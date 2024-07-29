#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include <getopt.h>
#include <dirent.h>
#define MAX_CONFIG_LINE_LENGTH 1000

// Set global variables to default values
int LOG_FILE_THRESHOLD = 1048576; // 1 MB
int MAX_LOG_FILES = 4;
#define SEM_NAME "logSyncSem"

sem_t *sem_ptr; // Global semaphore pointer

// Declaration of the functions
void error(const char *msg);
off_t getFileSize(const char *filename);
void deleteOldestLogFile(const char *directory);
int findMostRecentLogFile(const char *directory, char *mostRecentFile, size_t len);
int openMostRecentLogFile(const char *directory);
int findNumberOfLogFiles(const char *directory);
int readConfig(int *port, char *directory);
int createLogFile(const char *directory);
int rotateLog(const char *directory);
void clientHandler(int clientSocket, struct sockaddr_in clientAddr, const char *directory);
void logHandler(const char *message, const char *directory);
void serverListenLoop(int serverSocket, const char *logFileDirectory);
void getCurrentTime(char *timeStr);
void handleSigchild(int sig);
void handleSigUser1(int sig);
void handleSigUser2(int sig);
void handleSigINT(int sig);

volatile int n_connections = 0;
volatile int husr2 = 1;

// Declare a volatile flag for safely handling the termination of the program.
// 'volatile' tells the compiler the value of the variable can change at any time even in the presence of asynchronous interrupts made by signals.
volatile sig_atomic_t terminate = 0;

int main(int argc, char *argv[])
{
    int serverSocket, clientSocket, portNo;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    char logFileDirectory[128];
    char shutDownServer[128];
    char startUpServer[128];
    char startCloseMsg[256];

    // define the signls
    struct sigaction sigUsr1Action;
    struct sigaction sigchldAction;
    struct sigaction sigINTaction;
    sigUsr1Action.sa_handler = &handleSigUser1;
    sigchldAction.sa_handler = &handleSigchild;
    sigINTaction.sa_handler = &handleSigINT;
    sigaction(SIGCHLD, &sigchldAction, NULL);
    sigaction(SIGUSR1, &sigUsr1Action, NULL);
    sigaction(SIGINT, &sigINTaction, NULL);
    signal(SIGUSR2, SIG_IGN);

    // Initialize semaphore
    sem_ptr = sem_open(SEM_NAME, O_CREAT, 0644, 1);
    if (sem_ptr == SEM_FAILED)
    {
        error("Semaphore initialization failed");
    }

    // Check if command line arguments are provided
    if (argc != 3)
    {
        // No command line arguments, read configuration from a file
        if (readConfig(&portNo, logFileDirectory) != 0)
        {
            fprintf(stderr, "Error: Configuration file missing or invalid.\n");
            return 1;
        }
    }
    else
    {
        // Use command line arguments
        portNo = atoi(argv[1]);
        strcpy(logFileDirectory, argv[2]);
    }

    // Create a TCP socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        error("ERROR opening socket");
    }

    // Set up the server address structure
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(portNo);

    // Set socket option to allow immediate reuse of the address and port
    // This option enables the server to restart immediately after shutdown
    // without waiting for the TIME_WAIT period to expire.
    int optval = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    // Bind socket to an address

    if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
    {
        error("ERROR on binding");
    }

    // Listen for connections
    int listenfd = listen(serverSocket, 100);
    if (listenfd < 0)
    {
        error("Listen error");
    }
    // get the current time of starting up the server
    getCurrentTime(startUpServer);
    snprintf(startCloseMsg, sizeof(startCloseMsg), "[%s] Server start up.\n", startUpServer);
    // Write on the log file.
    logHandler(startCloseMsg, logFileDirectory);

    // Set server socket to non-blocking
    int flags = fcntl(serverSocket, F_GETFL, 0);
    // Make the socket non-blocking
    fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);
    // The main loop of the server
    serverListenLoop(serverSocket, logFileDirectory);
    // Get the current time of shutting down the server
    getCurrentTime(shutDownServer);
    snprintf(startCloseMsg, sizeof(startCloseMsg), "[%s] Server shut down.\n", shutDownServer);
    // Write on the log file.
    logHandler(startCloseMsg, logFileDirectory);
    return 0;
}

void serverListenLoop(int serverSocket, const char *logFileDirectory)
{
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int clientSocket;
    fd_set readfds;
    int max_sd;
    char buffer[1024];
    pid_t id = 0;

    while (!terminate)
    {
        FD_ZERO(&readfds);
        // Add server socket to set
        FD_SET(serverSocket, &readfds);
        max_sd = serverSocket;

        // Add standard input to set
        FD_SET(STDIN_FILENO, &readfds);
        if (STDIN_FILENO > max_sd)
        {
            max_sd = STDIN_FILENO;
        }

        // Wait for an activity on one of the sockets, timeout is NULL, so wait indefinitely
        int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR))
        {
            printf("select error");
        }
        else if (activity > 0)
        {
            // If something happened on stdin, check for quit command
            if (FD_ISSET(STDIN_FILENO, &readfds))
            {
                int readMsg = read(STDIN_FILENO, buffer, sizeof(buffer));
                // ctrl+d perfomed, the server has to quit
                if (readMsg == 0)
                {
                    break;
                }
                if (strncmp(buffer, "quit", 4) == 0)
                {
                    break;
                }
                else
                {
                    memset(buffer, 0, sizeof(buffer));
                }
            }

            // If something happened on the server socket, then it's an incoming connection
            if (FD_ISSET(serverSocket, &readfds))
            {
                clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
                if (clientSocket < 0)
                {
                    perror("ERROR on accept");
                    continue;
                }
                if (clientSocket != -1)
                {
                    id = fork();
                    if (id == -1)
                    {
                        close(clientSocket);
                    }
                    else if (id == 0)
                    {
                        // ignoring SIGUSR1 and SIGINT, that are handled from the parent process
                        signal(SIGUSR1, SIG_IGN);
                        signal(SIGINT, SIG_IGN);

                        // redefining the default behavior when a child receive a SIGUSR2 signal.
                        struct sigaction sigUsr2Action;
                        sigUsr2Action.sa_handler = &handleSigUser2;
                        sigaction(SIGUSR2, &sigUsr2Action, NULL);

                        // This is the child process
                        clientHandler(clientSocket, clientAddr, logFileDirectory);
                    }
                    else
                    {
                        // Parent process
                        close(clientSocket);
                        n_connections++;
                    }
                }
            }
        }
    }
    // the server ignore every SIGCHLD signal
    signal(SIGCHLD, SIG_IGN);
    int kchild = kill(0, SIGUSR2);
    // waiting for all the children to quit the process
    while (1)
    {
        pid_t child = waitpid(-1, NULL, WNOHANG);
        if (child == -1)
        {
            if (errno != EINTR)
            { // errno == ECHILD, no more children, we can break.
                break;
            }
        }
        else if (child > 0)
        {
            --n_connections;
        }
    }
    write(STDOUT_FILENO, "Server is closed.\n", 19);
    // Clean up
    sem_destroy(sem_ptr);
    shutdown(serverSocket, SHUT_RDWR);
    close(serverSocket);
    sem_unlink(SEM_NAME);
}
// Function for handling errors and exiting the program.
void error(const char *msg)
{
    perror(msg); // Print the error message passed to the function along with the system error message.
    exit(1);     // Exit the program with a non-zero status, indicating that an error occurred.
}

// Function to get the size of a file.
off_t getFileSize(const char *filename)
{
    struct stat st; // Declare a stat struct to store file information.

    // Call 'stat' on the filename. If successful, 'stat' fills in 'st' with file details.
    if (stat(filename, &st) == 0)
    {
        return st.st_size; // Return the size of the file (in bytes)
    }
    else
    {
        return -1; // Error occurred
    }
}

// Function to find and delete the oldest log file in a given directory
void deleteOldestLogFile(const char *directory)
{
    DIR *dir;
    struct dirent *entry;
    struct tm oldestTime;
    char oldestFile[128];
    int firstFile = 1;

    dir = opendir(directory);
    if (!dir)
    {
        perror("Error opening directory");
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG && strstr(entry->d_name, "server_log_") == entry->d_name)
        {
            struct tm fileTime = {0};
            sscanf(entry->d_name, "server_log_%d-%d-%d|%d:%d:%d",
                   &fileTime.tm_year, &fileTime.tm_mon, &fileTime.tm_mday,
                   &fileTime.tm_hour, &fileTime.tm_min, &fileTime.tm_sec);
            fileTime.tm_year -= 1900;
            fileTime.tm_mon -= 1;

            if (firstFile || difftime(mktime(&fileTime), mktime(&oldestTime)) < 0)
            {
                strncpy(oldestFile, entry->d_name, sizeof(oldestFile));
                oldestTime = fileTime;
                firstFile = 0;
            }
        }
    }

    closedir(dir);

    // Delete the oldest file
    if (!firstFile)
    {
        char filePath[256];
        snprintf(filePath, sizeof(filePath), "%s/%s", directory, oldestFile);
        if (unlink(filePath) != 0)
        {
            perror("Error deleting oldest file");
        }
    }
}
// Function to find the most recent log file in a given directory

int findMostRecentLogFile(const char *directory, char *mostRecentFile, size_t len)
{
    DIR *dir;                   // Directory stream
    struct dirent *entry;       // Structure representing directory entry
    struct tm latestTime = {0}; // Store the time of the latest file found
    int firstFile = 1;          // Flag to check if it's the first file
    int flag = 0;

    dir = opendir(directory); // Open the specified directory
    if (!dir)
    {
        perror("Error opening directory");
        return -1;
    }
    int x = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        // Check if entry is a regular file and starts with "server_log_"
        if (entry->d_type == DT_REG && strstr(entry->d_name, "server_log_") == entry->d_name)
        {
            flag = 1;
            struct tm fileTime = {0}; // Temporarily store the file's timestamp
            // Parse the timestamp from the file name
            sscanf(entry->d_name, "server_log_%d-%d-%d|%d:%d:%d",
                   &fileTime.tm_year, &fileTime.tm_mon, &fileTime.tm_mday,
                   &fileTime.tm_hour, &fileTime.tm_min, &fileTime.tm_sec);
            fileTime.tm_year -= 1900; // Adjust year
            fileTime.tm_mon -= 1;     // Adjust month

            // Compare the parsed time with the latest time found
            if (firstFile || difftime(mktime(&fileTime), mktime(&latestTime)) > 0)
            {
                strncpy(mostRecentFile, entry->d_name, len); // Update the most recent file
                latestTime = fileTime;                       // Update the latest time
                firstFile = 0;                               // Clear the first file flag
            }
        }
    }

    closedir(dir);
    return (flag);
}

// Function to open the most recent log file in a given directory
int openMostRecentLogFile(const char *directory)
{
    char mostRecentFile[128];
    int isRecFileExist = findMostRecentLogFile(directory, mostRecentFile, sizeof(mostRecentFile));

    if (isRecFileExist)
    {
        char filePath[256];
        snprintf(filePath, sizeof(filePath), "%s/%s", directory, mostRecentFile);
        int fd = open(filePath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
        {
            perror("Error opening most recent log file");
            return -1; // Error occurred
        }
        return fd; // Return the file descriptor
    }
    return -1; // No recent file found
}

// Function to find the number of log files in a given directory
int findNumberOfLogFiles(const char *directory)
{
    DIR *dir;             // Directory stream
    struct dirent *entry; // Structure representing directory entry
    int numberOfLogFiles = 0;

    dir = opendir(directory); // Open the specified directory
    if (!dir)
    {
        perror("Error opening directory");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        // Check if entry is a regular file and starts with "server_log_"
        if (entry->d_type == DT_REG && strstr(entry->d_name, "server_log_") == entry->d_name)
        {
            numberOfLogFiles++;
        }
    }

    closedir(dir);
    return numberOfLogFiles;
}

// Function to read server configuration from a file.
int readConfig(int *port, char *directory)
{
    // Open the configuration file in read-only mode.
    int fd = open("config.txt", O_RDONLY);
    char buffer[MAX_CONFIG_LINE_LENGTH];
    ssize_t bytes_read;
    char *line;
    char key[MAX_CONFIG_LINE_LENGTH];
    char value[MAX_CONFIG_LINE_LENGTH];

    // Check if the file was successfully opened.
    if (fd < 0)
    {
        error("Error opening config file");
    }

    // Read the contents of the file into 'buffer'.
    bytes_read = read(fd, buffer, sizeof(buffer) - 1);

    // Check if the read operation was successful.
    if (bytes_read <= 0)
    {
        close(fd);
        error("Error reading config file");
    }

    buffer[bytes_read] = '\0'; // Null-terminate the buffer to create a valid C string.

    line = strtok(buffer, "\n"); // Tokenize the buffer to read it line by line.

    while (line != NULL)
    {

        sscanf(line, "%[^=]=%s", key, value); // Parse key=value
        // If the key is "port", convert the value to integer and store it in 'port'.
        if (strcmp(key, "port") == 0)
        {
            *port = atoi(value);
        }
        // If the key is "directory", copy the value to 'directory'
        else if (strcmp(key, "directory") == 0)
        {
            strcpy(directory, value);
        }
        else if (strcmp(key, "log_file_threshold") == 0)
        {
            LOG_FILE_THRESHOLD = atoi(value);
        }
        else if (strcmp(key, "max_log_files") == 0)
        {
            MAX_LOG_FILES = atoi(value);
        }
        line = strtok(NULL, "\n"); // Move to the next line.
    }

    // Close the file descriptor.
    close(fd);

    return 0;
}

// Function to create a new log file in the specified directory.It returns a file descriptor to the opened log file.
int createLogFile(const char *directory)
{
    char filename[40];
    char filepath[100];

    // Get the current time.
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    // Format the log file name with the current timestamp.
    strftime(filename, 40, "server_log_%Y-%m-%d|%H:%M:%S.txt", tm_info);

    // Construct the full path to the log file.
    snprintf(filepath, sizeof(filepath), "%s/%s", directory, filename);

    // Open the log file for writing; create it if it doesn't exist; append if it does.
    int log_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0)
    {
        error("Error opening log file");
    }
    return log_fd;
}

// Function to perform log rotation
int rotateLog(const char *directory)
{
    if (findNumberOfLogFiles(directory) >= MAX_LOG_FILES)
    {
        deleteOldestLogFile(directory);
    }
    int log_fd = createLogFile(directory);
    return log_fd;
}

// Function to handle new clients
void clientHandler(int clientSocket, struct sockaddr_in clientAddr, const char *directory)
{
    char buffer[1024];
    char connectionMessage[1024];
    char logMessage[2048];
    char clientName[256];
    char timeStr[128];
    int bytesRead;
    int sem_wait_ret;
    fd_set s_rd;
    char log_entry_string[1150] = {'\0'};

    int client_disconnected = 0;
    // Get client IP address
    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

    buffer[strcspn(buffer, "\n")] = 0;
    bytesRead = read(clientSocket, clientName, sizeof(clientName) - 1);
    if (bytesRead <= 0)
    {
        perror("ERROR in Reading from client.\n");
    }
    clientName[bytesRead] = '\0'; // Null-terminate the string
    getCurrentTime(timeStr);

    // Log client name
    snprintf(connectionMessage, sizeof(connectionMessage), "[%s] Client (IP: %s, name: %s) is connected.\n", timeStr, clientIP, clientName);

    logHandler(connectionMessage, directory);
    while (husr2)
    {
        FD_SET(clientSocket, &s_rd);
        int select_socket_fd = select(clientSocket + 1, &s_rd, NULL, NULL, NULL);
        if (select_socket_fd == -1)
        {
            if (errno != EINTR)
            {
                perror("Select failure");
            }
        }
        else if (select_socket_fd)
        {

            bytesRead = read(clientSocket, buffer, 1023);
            buffer[bytesRead] = '\0';
            getCurrentTime(timeStr);

            buffer[strcspn(buffer, "\n")] = 0;

            if (strcmp(buffer, "quit") == 0)
            {
                // Construct the log message for "quit" command
                char quitLogMessage[1024];
                snprintf(quitLogMessage, sizeof(quitLogMessage), "[%s] Client (IP: %s, name: %s) sent quit command.\n", timeStr, clientIP, clientName);

                logHandler(quitLogMessage, directory);
                close(clientSocket);

                // Terminate the child process
                exit(0);
            }
            // Construct the log message
            snprintf(logMessage, sizeof(logMessage), "[%s] Client (%s) - %s: %s\n", timeStr, clientIP, clientName, buffer);
            logHandler(logMessage, directory);
            if (bytesRead == 0)
            {
                husr2 = 0;
            }

            do
            {
                sem_wait_ret = sem_wait(sem_ptr);
            } while (sem_wait_ret == -1 && errno == EINTR);
            int stout_w = write(STDOUT_FILENO, log_entry_string, strlen(log_entry_string));
            if (bytesRead == 0)
            {

                logHandler(log_entry_string, directory);
            }

            sem_post(sem_ptr);
        }
        memset(log_entry_string, 0, sizeof(log_entry_string));
        memset(buffer, 0, sizeof(buffer));
        FD_ZERO(&s_rd);
    }

    close(clientSocket);
    exit(EXIT_SUCCESS);
}

// Function to manage writing on log file
void logHandler(const char *logMessage, const char *directory)
{
    // Wait on the semaphore to gain access to the critical section
    sem_wait(sem_ptr);
    int bytesRead, log_fd = -1;
    char recentLogFile[128];
    char recentLogFilePath[256];
    log_fd = openMostRecentLogFile(directory);
    if (log_fd == -1)
    {
        log_fd = createLogFile(directory);
    }

    int w = write(log_fd, logMessage, strlen(logMessage));
    if (w <= 0)
    {
        close(log_fd);
        error("Error writing.");
    }

    // Check log file size and rotate if necessary
    findMostRecentLogFile(directory, recentLogFile, sizeof(recentLogFile));
    snprintf(recentLogFilePath, sizeof(recentLogFilePath), "%s/%s", directory, recentLogFile);

    if (getFileSize(recentLogFilePath) > LOG_FILE_THRESHOLD)
    {
        close(log_fd);
        log_fd = rotateLog(directory);
    }

    if (log_fd != -1)
    {
        close(log_fd);
    }
    sem_post(sem_ptr); // Signal semaphore
}

// Function to handle the quit signals that are coming from the clients
void handleSigchild(int sig)
{
    int childPID, childExitStatus;
    printf("\nSIGCHILD signal received\n");
    n_connections--;
    while ((childPID = waitpid(-1, &childExitStatus, WNOHANG)) > 0)
    {
        if (childExitStatus == 2)
        {
            printf("Background process: %d%s", childPID, " terminated by SIGINT\n");
        }
        else if (childExitStatus != 0)
        {
            printf("Background process: %d%s", childPID, " unknown command\n");
        }
        printf("Background process: %d%s.\n Number of clients still connected: %d.\n", childPID, " has exited", n_connections);
    }
}

// Signale handler for parent process
void handleSigUser1(int sig)
{
    terminate = 1;
}
// Signale handler for child process
void handleSigUser2(int sig)
{
    husr2 = 0;
}
// Signale handler for Ctrl+C
void handleSigINT(int sig)
{
    printf("Ctrl+C Received!\n");
    terminate = 1;
}

// Function to get the current time as a formatted string
void getCurrentTime(char *timeStr)
{
    time_t now = time(NULL);               // Get the system time
    struct tm *timeinfo = localtime(&now); // Convert to local time

    // Format the time string
    strftime(timeStr, 100, "[%Y-%m-%d %H:%M:%S]", timeinfo);
}