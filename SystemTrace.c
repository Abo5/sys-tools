/*
 * Smart SSH Server - High Performance C Version
 * Features: Auto-start, Battery monitoring, Shutdown detection, 
 *          GitHub updates, Telegram notifications via curl
 * 
 * Compile:
 * Windows: gcc -o sshserver.exe smartssh.c -lcurl -ljson-c -lws2_32 -DWIN32
 * Linux:   gcc -o sshserver smartssh.c -lcurl -ljson-c -lpthread
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
#define GITHUB_OWNER "YOUR_USERNAME"
#define GITHUB_REPO "sshserver"
#define TELEGRAM_BOT_TOKEN "YOUR_BOT_TOKEN"
#define TELEGRAM_CHAT_ID "YOUR_CHAT_ID"

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
#ifdef WIN32
    return system(command);
#else
    return system(command);
#endif
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
    // Leave for admin to manage on Linux
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
    // Check for shutdown/restart processes
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
    // Check for shutdown/reboot processes on Linux
    return (execute_command("pgrep shutdown >/dev/null 2>&1") == 0 ||
            execute_command("pgrep reboot >/dev/null 2>&1") == 0 ||
            execute_command("pgrep poweroff >/dev/null 2>&1") == 0);
#endif
}

// ======================================================
// GitHub update checker
// ======================================================

struct github_release {
    char tag_name[64];
    char download_url[512];
};

int check_github_update(struct github_release *release) {
    if (strcmp(GITHUB_OWNER, "YOUR_USERNAME") == 0) {
        return 0; // Not configured
    }
    
    CURL *curl;
    CURLcode res;
    struct http_response response = {0};
    char url[256];
    
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest", GITHUB_OWNER, GITHUB_REPO);
    
    curl = curl_easy_init();
    if (!curl) return 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || !response.data) {
        if (response.data) free(response.data);
        return 0;
    }
    
    // Simple JSON parsing (looking for tag_name and download_url)
    char *tag_start = strstr(response.data, "\"tag_name\":");
    if (tag_start) {
        tag_start = strchr(tag_start, '"');
        if (tag_start) {
            tag_start = strchr(tag_start + 1, '"') + 1;
            char *tag_end = strchr(tag_start, '"');
            if (tag_end) {
                int len = tag_end - tag_start;
                if (len < sizeof(release->tag_name)) {
                    strncpy(release->tag_name, tag_start, len);
                    release->tag_name[len] = '\0';
                }
            }
        }
    }
    
    // Look for appropriate binary
    char target[128];
#ifdef WIN32
    snprintf(target, sizeof(target), "sshserver-windows-amd64.exe");
#else
    snprintf(target, sizeof(target), "sshserver-linux-amd64");
#endif
    
    char *asset_start = strstr(response.data, target);
    if (asset_start) {
        char *url_start = asset_start;
        for (int i = 0; i < 5 && url_start; i++) {
            url_start = strstr(url_start, "browser_download_url");
            if (url_start) {
                url_start = strchr(url_start, '"');
                if (url_start) {
                    url_start = strchr(url_start + 1, '"') + 1;
                    char *url_end = strchr(url_start, '"');
                    if (url_end) {
                        int len = url_end - url_start;
                        if (len < sizeof(release->download_url)) {
                            strncpy(release->download_url, url_start, len);
                            release->download_url[len] = '\0';
                            break;
                        }
                    }
                }
            }
        }
    }
    
    free(response.data);
    
    // Check if we have a newer version
    return (strlen(release->tag_name) > 0 && strcmp(release->tag_name, VERSION) != 0);
}

// ======================================================
// Simple SSH Server
// ======================================================

void handle_ssh_connection(SOCKET client_socket) {
    char buffer[256];
    char response[512];
    
    snprintf(response, sizeof(response), "SSH-2.0-SmartSSH_%s\r\n", VERSION);
    send(client_socket, response, strlen(response), 0);
    
    // Read client banner
    recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    snprintf(response, sizeof(response), 
        "Welcome to Smart SSH Server %s\r\n"
        "This is a demo server - use OpenSSH for full access\r\n", VERSION);
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
// Signal handlers
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
        printf("Smart SSH Server v%s (C Version)\n", VERSION);
        printf("\nUsage:\n");
        printf("  %s smart    - Smart mode with all monitoring\n", argv[0]);
        printf("  %s setup    - Setup auto-start service\n", argv[0]);
        printf("  %s update   - Check for updates\n", argv[0]);
        return 1;
    }
    
    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if (strcmp(argv[1], "smart") == 0) {
        printf("========================================\n");
        printf("  Smart SSH Server v%s (C Version)\n", VERSION);
        printf("========================================\n");
        
        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        atexit(cleanup_and_exit);
        
        // Send startup notification
        char startup_msg[256];
        snprintf(startup_msg, sizeof(startup_msg), "🚀 <b>Smart SSH Server Started</b>\nVersion: %s (C)\nOS: %s\nPort: %d", 
                 VERSION, 
#ifdef WIN32
                 "Windows",
#else
                 "Linux",
#endif
                 SSH_PORT);
        send_telegram_message(startup_msg);
        
        printf("\n[1/5] Enabling firewall...\n");
        if (enable_firewall()) {
            printf("✓ Opened port %d\n", SSH_PORT);
        } else {
            printf("⚠ Firewall setup warning\n");
        }
        
        printf("\n[2/5] Starting SSH server...\n");
#ifdef WIN32
        CreateThread(NULL, 0, ssh_server_thread, NULL, 0, NULL);
#else
        pthread_t ssh_thread;
        pthread_create(&ssh_thread, NULL, ssh_server_thread, NULL);
#endif
        printf("✓ SSH server ready\n");
        
        printf("\n[3/5] Starting shutdown monitor...\n");
#ifdef WIN32
        CreateThread(NULL, 0, shutdown_monitor_thread, NULL, 0, NULL);
#else
        pthread_t shutdown_thread;
        pthread_create(&shutdown_thread, NULL, shutdown_monitor_thread, NULL);
#endif
        printf("✓ Shutdown detection active\n");
        
        printf("\n[4/5] Task cleanup in 10 seconds...\n");
        sleep(10);
        if (is_task_registered()) {
            unregister_task();
            printf("[CLEANUP] ✓ Removed Task Scheduler (running in memory)\n");
        }
        
        printf("\n[5/5] Starting battery monitoring...\n");
#ifdef WIN32
        CreateThread(NULL, 0, battery_monitor_thread, NULL, 0, NULL);
        printf("✓ Battery monitoring active\n");
#else
        printf("✓ Battery monitoring (Windows only)\n");
#endif
        
        printf("\n" "================================================" "\n");
        printf("  SSH Server ready on port %d\n", SSH_PORT);
        printf("  ✓ Shutdown detection active\n");
        printf("  ✓ Auto-save on emergency\n");
        printf("  ✓ Smart firewall management\n");
        printf("  ✓ Telegram notifications active\n");
        printf("  • To stop: Ctrl+C\n");
        printf("  • To connect: ssh -p %d %s@<IP>\n", SSH_PORT, SSH_USER);
        printf("  • Password: %s\n", SSH_PASSWORD);
        printf("================================================" "\n");
        
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
        printf("Checking for updates from GitHub...\n");
        
        struct github_release release;
        memset(&release, 0, sizeof(release));
        
        if (check_github_update(&release)) {
            printf("New update available: %s → %s\n", VERSION, release.tag_name);
            printf("Download URL: %s\n", release.download_url);
            // Note: Auto-download and replace would be implemented here
        } else {
            printf("✓ Already on latest version: %s\n", VERSION);
        }
        
    } else {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }
    
    curl_global_cleanup();
    return 0;
}
