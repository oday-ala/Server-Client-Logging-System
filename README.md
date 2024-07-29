# **Server-Client Logging System**

**The Server-Client Logging System is designed to manage and log messages from multiple clients concurrently.**

## Key Features:
-  Multi-client Handling:

> The server uses fork() to handle multiple clients. Each client connection is managed in a separate child process, allowing the server to handle multiple connections simultaneously.
- Concurrency Control:

>Semaphores are used to synchronize access to log files. This ensures that multiple child processes can write to the log files concurrently without data corruption.
- Logging:

>Messages sent from clients are logged to a specified directory. The log files are managed by the server and are named with timestamps for easy identification.

## Installation

1. Ensure you have a C compiler (gcc version 11.4.0) installed on Ubuntu 22.04.1.
2. Obtain the project files (server.c, client.c, config.txt) and ensure they are in the same directory.

## Compilation
Open the terminal in the project directory and compile the server and client applications using the following commands:
```
# For the server:
gcc server.c -o server -pthread

# For the client:
gcc client.c -o client
```

## Configuration
Before running the server, configure the config.txt file with the desired port, directory for logs, and other settings:
```
port=<port>
directory=<log_file_directory>
ip_address= <ip_address>
log_file_threshold=<log_file_threshold>
max_log_files=<max_log_files_in_the_directory>
```



## Usage

- To start the server, run: ```./server <port> <log_file_directory>```

   If you omit <port> and <log_file_directory>, the server will use the settings from config.txt.


- Running the Client
Open a new terminal and start the client application by running:
```./client <server_ip> <port>```

   Replace <server_ip> and <port> with the server's IP address and port number. If omitted, the client uses config.txt settings.

## Testing
Once both server and client are running, you can send messages from the client terminal. These messages are logged by the server. 
To stop the client, type ```quit```. 
To terminate the server, type ```quit``` or send a SIGINT signal (```Ctrl+C``` in the terminal).
