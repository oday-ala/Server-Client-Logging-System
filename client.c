#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#define MAX_CONFIG_LINE_LENGTH 256

// Declaration of the functions
void error(const char *msg);
int readConfig(int *port, char *ip_address);
// Global flag to indicate if the client should terminate
volatile sig_atomic_t terminate = 0;
// Signal handler function to handle termination signal
void handle_shutdown(int signum)
{
    if (signum == SIGINT || signum == SIGUSR2)
    {
        terminate = 1;
    }
}
int main(int argc, char *argv[])
{
    int sockfd, portNo;
    struct sockaddr_in serverAddr;
    char buffer[1024];
    struct hostent *server;
    char ip_address[100];
    char server_address[100];
    char name[256];

    // Check if command line arguments are provided
    if (argc != 3)
    {
        // No command line arguments, read configuration from a file
        if (readConfig(&portNo, ip_address) == 0)
        {
            server = gethostbyname(ip_address);
        }
        else
        {
            error("Error reading config file.");
        }
    }
    else
    {
        portNo = atoi(argv[2]);
        // Specify server address
        server = gethostbyname(argv[1]);
        if (server == NULL)
        {
            error("Error in hostname!\n");
        }
    }

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        error("ERROR opening socket");
    }
    signal(SIGINT, handle_shutdown); // Handle Ctrl+C
    signal(SIGUSR2, handle_shutdown);
    // Set up the server address structure
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(portNo);
    bcopy((char *)server->h_addr, (char *)&serverAddr.sin_addr.s_addr, server->h_length);

    // Connect to the server
    if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
    {
        error("ERROR connecting");
    }
    printf("Enter your name: ");
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = 0; // Remove the newline character

    // Send name to the server
    write(sockfd, name, strlen(name));
    fcntl(sockfd, F_SETFL, O_NONBLOCK);
    bzero(buffer, 1024);
    // Send messages to the server
    while (!terminate)
    {

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        // Wait for activity on the socket or standard input
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0)
        {
            error("ERROR in select");
        }

        // Check if there is data to read from the socket
        if (FD_ISSET(sockfd, &readfds))
        {
            ssize_t bytesRead = read(sockfd, buffer, sizeof(buffer) - 1);
            if (bytesRead < 0)
            {
                // Handle read error
                perror("ERROR reading from server");
                break;
            }
            else if (bytesRead == 0)
            {
                // Server has closed the connection
                printf("Server has closed the connection. Exiting...\n");
                break;
            }
            bzero(buffer, 1024);
        }

        if (fgets(buffer, 1023, stdin) != NULL)
        {
            buffer[strcspn(buffer, "\n")] = 0;
            write(sockfd, buffer, strlen(buffer));
            if (strncmp(buffer, "quit", 4) == 0)
            {
                break; // Break out of the loop if "quit" is entered
            }
            bzero(buffer, 1024);
        }
        else if (feof(stdin))
        {
            printf("EOF reached on stdin. Exiting...\n");
            break;
        }
        else if (ferror(stdin))
        {
            perror("Error reading from stdin");
            break;
        }

        if (terminate)
        {
            printf("Received termination signal. Exiting...\n");
            break;
        };
    }

    printf("Closing the connection to the server...\n");
    close(sockfd);
    return 0;
}

// Function for handling errors and exiting the program.
void error(const char *msg)
{
    perror(msg); // Print the error message passed to the function along with the system error message.
    exit(1);     // Exit the program with a non-zero status, indicating that an error occurred.
}

// Function to read server configuration from a file.
int readConfig(int *port, char *ip_address)
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
        perror("Error opening config file");
        return -1;
    }

    // Read the contents of the file into 'buffer'.
    bytes_read = read(fd, buffer, sizeof(buffer) - 1);

    // Check if the read operation was successful.
    if (bytes_read <= 0)
    {
        close(fd);
        perror("Error reading config file");
        return -1;
    }

    buffer[bytes_read] = '\0';   // Null-terminate the string
    line = strtok(buffer, "\n"); // Tokenize the buffer by new lines

    while (line != NULL)
    {
        sscanf(line, "%[^=]=%s", key, value); // Parse key=value

        if (strcmp(key, "port") == 0)
        {
            *port = atoi(value);
        }
        else if (strcmp(key, "ip_address") == 0)
        {
            strcpy(ip_address, value);
        }

        line = strtok(NULL, "\n"); // Get next line
    }

    // Close the file descriptor.
    close(fd);
    return 0;
}
