#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>
#include <errno.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define S1_IP "127.0.0.1"  // Use localhost for testing
#define S1_PORT 8080
#define MAX_BUFF 4096

// Function prototypes
int validate_command(char *cmd, char *arg1, char *arg2);
int upload_command_validation(char *filename, char *dest_path);
int download_command_validation(char *filepath);
int remove_command_validation(char *filepath);
int downloadtar_command_validation(char *filetype);
int display_command_validation(char *pathname);
void send_file(int sock, char *filename);
void receive_file(int sock, char *filename);
void receive_tar(int sock, char *filetype);
void receive_filenames(int sock);
void print_help();
int connect_to_server();

int main(int argc, char const *argv[]) {
    char command[MAX_BUFF];
    char response[MAX_BUFF];
    
    printf("W25 Distributed File System Client\n");
    printf("Type 'help' for available commands\n");
    printf("w25client$ ");
    
    while (fgets(command, MAX_BUFF, stdin)) {
        // Save original command for sending
        char original_cmd[MAX_BUFF];
        strcpy(original_cmd, command);
        
        // Remove newline for local validation
        size_t len = strlen(command);
        if (len > 0 && command[len - 1] == '\n')
            command[len - 1] = '\0';
            
        if (strlen(command) == 0) {
            printf("w25client$ ");
            continue;
        }
        
        // Parse command for validation
        char cmd_copy[MAX_BUFF];
        strcpy(cmd_copy, command);
        
        char *cmd = strtok(cmd_copy, " ");
        char *arg1 = strtok(NULL, " ");
        char *arg2 = strtok(NULL, " ");
        
        // Check for help command
        if (strcmp(cmd, "help") == 0) {
            print_help();
            printf("w25client$ ");
            continue;
        }
        
        // Check for exit/quit command
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            printf("Exiting client...\n");
            break;
        }
        
        // Validate command syntax
        int valid = validate_command(cmd, arg1, arg2);
        
        if (!valid) {
            printf("w25client$ ");
            continue;
        }
        
        // Connect to server for this command
        int sock = connect_to_server();
        if (sock < 0) {
            printf("Failed to connect to server. Please try again.\n");
            printf("w25client$ ");
            continue;
        }
        
        // Send command
        printf("Sending command: %s", original_cmd);
        if (len > 0 && original_cmd[len - 1] != '\n') {
            // Ensure command ends with newline
            send(sock, command, strlen(command), 0);
            send(sock, "\n", 1, 0);
        } else {
            send(sock, original_cmd, strlen(original_cmd), 0);
        }
        
        // Handle file upload
        if (strcmp(cmd, "uploadf") == 0) {
            printf("Uploading file: %s\n", arg1);
            send_file(sock, arg1);
        }
        
        // Handle file download
        if (strcmp(cmd, "downlf") == 0) {
            // Extract filename from path
            char *filename = strrchr(arg1, '/');
            if (!filename) {
                filename = arg1;
            } else {
                filename++; // Skip the '/'
            }
            receive_file(sock, filename);
        }
        
        // Handle tar download
        if (strcmp(cmd, "downltar") == 0) {
            receive_tar(sock, arg1);
        }
        
        // Handle display filenames
        if (strcmp(cmd, "dispfnames") == 0) {
            receive_filenames(sock);
        }
        
        // Receive server response for commands that don't have special handling
        if (strcmp(cmd, "removef") == 0) {
            fd_set readfds;
            struct timeval tv;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            tv.tv_sec = 5;  // 5 second timeout
            tv.tv_usec = 0;
            
            if (select(sock + 1, &readfds, NULL, NULL, &tv) > 0) {
                memset(response, 0, MAX_BUFF);  // Clear the buffer
                ssize_t bytes_received = recv(sock, response, MAX_BUFF - 1, 0);
                if (bytes_received <= 0) {
                    printf("Server disconnected\n");
                } else {
                    response[bytes_received] = '\0';
                    printf("Server: %s\n", response);
                }
            } else {
                printf("No response from server (timeout)\n");
            }
        }
        
        // Close connection for this command
        close(sock);
        printf("w25client$ ");
    }
    
    return 0;
}

// Validate different command types
int validate_command(char *cmd, char *arg1, char *arg2) {
    if (!cmd) {
        printf("Error: No command provided\n");
        return 0;
    }
    
    if (strcmp(cmd, "uploadf") == 0) {
        if (!arg1 || !arg2) {
            printf("Usage: uploadf <filename> <destination_path>\n");
            return 0;
        }
        return upload_command_validation(arg1, arg2);
    } else if (strcmp(cmd, "downlf") == 0) {
        if (!arg1) {
            printf("Usage: downlf <filepath>\n");
            return 0;
        }
        return download_command_validation(arg1);
    } else if (strcmp(cmd, "removef") == 0) {
        if (!arg1) {
            printf("Usage: removef <filepath>\n");
            return 0;
        }
        return remove_command_validation(arg1);
    } else if (strcmp(cmd, "downltar") == 0) {
        if (!arg1) {
            printf("Usage: downltar <filetype>\n");
            printf("where filetype is: c (C source), p (PDF), t (text), or z (ZIP)\n");
            return 0;
        }
        return downloadtar_command_validation(arg1);
    } else if (strcmp(cmd, "dispfnames") == 0) {
        if (!arg1) {
            printf("Usage: dispfnames <pathname>\n");
            return 0;
        }
        return display_command_validation(arg1);
    } else {
        printf("Error: Unknown command '%s'\n", cmd);
        printf("Type 'help' for available commands\n");
        return 0;
    }
}

int upload_command_validation(char *filename, char *dest_path) {
    struct stat st;
    
    if (stat(filename, &st) != 0) {
        printf("Error: File '%s' not found\n", filename);
        return 0;
    }
    
    char *ext = strrchr(filename, '.');
    if (!ext || (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
                strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0)) {
        printf("Error: Invalid file extension. Only .c, .pdf, .txt, and .zip are supported\n");
        return 0;
    }
    
    // Validate destination path starts with ~S1/
    if (strncmp(dest_path, "~S1/", 4) != 0) {
        printf("Error: Destination path must start with ~S1/\n");
        return 0;
    }
    
    return 1;
}

int download_command_validation(char *filepath) {
    // Validate filepath starts with ~S1/
    if (strncmp(filepath, "~S1/", 4) != 0) {
        printf("Error: File path must start with ~S1/\n");
        return 0;
    }
    
    // Extract filename from path to verify extension
    char *filename = strrchr(filepath, '/');
    if (!filename) {
        printf("Error: Invalid file path format\n");
        return 0;
    }
    filename++; // Skip the '/'
    
    char *ext = strrchr(filename, '.');
    if (!ext || (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
                strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0)) {
        printf("Error: Invalid file extension. Only .c, .pdf, .txt, and .zip are supported\n");
        return 0;
    }
    
    return 1;
}

int remove_command_validation(char *filepath) {
    // Validate filepath starts with ~S1/
    if (strncmp(filepath, "~S1/", 4) != 0) {
        printf("Error: File path must start with ~S1/\n");
        return 0;
    }
    
    // Extract filename from path to verify extension
    char *filename = strrchr(filepath, '/');
    if (!filename) {
        printf("Error: Invalid file path format\n");
        return 0;
    }
    filename++; // Skip the '/'
    
    char *ext = strrchr(filename, '.');
    if (!ext || (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
                strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0)) {
        printf("Error: Invalid file extension. Only .c, .pdf, .txt, and .zip are supported\n");
        return 0;
    }
    
    return 1;
}

int downloadtar_command_validation(char *filetype) {
    if (strcmp(filetype, "c") != 0 && strcmp(filetype, "p") != 0 &&
        strcmp(filetype, "t") != 0 && strcmp(filetype, "z") != 0) {
        printf("Error: Invalid file type. Use: c (C source), p (PDF), t (text), or z (ZIP)\n");
        return 0;
    }
    return 1;
}

int display_command_validation(char *pathname) {
    // Validate pathname starts with ~S1/
    if (strncmp(pathname, "~S1/", 4) != 0) {
        printf("Error: Path must start with ~S1/\n");
        return 0;
    }
    return 1;
}

void send_file(int sock, char *filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        perror("Cannot stat file");
        return;
    }
    
    off_t file_size = st.st_size;
    printf("File size: %ld bytes\n", file_size);
    
    // Send file size header with newline
    char size_header[32];
    snprintf(size_header, sizeof(size_header), "%ld\n", file_size);
    send(sock, size_header, strlen(size_header), 0);
    
    // Send file content
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Cannot open file");
        return;
    }
    
    char buffer[MAX_BUFF];
    ssize_t bytes_read;
    off_t total_sent = 0;
    
    while ((bytes_read = read(fd, buffer, MAX_BUFF)) > 0) {
        ssize_t bytes_sent = send(sock, buffer, bytes_read, 0);
        if (bytes_sent < 0) {
            perror("Send error");
            break;
        }
        total_sent += bytes_sent;
        
        // Show progress
        float percent = (float)total_sent / file_size * 100.0;
        printf("\rProgress: %.1f%% (%ld/%ld bytes)", percent, total_sent, file_size);
        fflush(stdout);
    }
    
    close(fd);
    printf("\nFile transfer complete: %ld/%ld bytes\n", total_sent, file_size);
    
    // Receive server response
    char response[MAX_BUFF];
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = 10;  // 10 second timeout
    tv.tv_usec = 0;
    
    if (select(sock + 1, &readfds, NULL, NULL, &tv) > 0) {
        memset(response, 0, MAX_BUFF);
        ssize_t bytes_received = recv(sock, response, MAX_BUFF - 1, 0);
        if (bytes_received > 0) {
            response[bytes_received] = '\0';
            printf("Server: %s\n", response);
        }
    }
}

void receive_file(int sock, char *filename) {
    char size_buf[32];
    ssize_t bytes_read = 0;
    int i = 0;
    
    // Read file size (with timeout)
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    
    if (select(sock + 1, &readfds, NULL, NULL, &tv) <= 0) {
        printf("Timeout waiting for server response\n");
        return;
    }
    
    // Read size character by character until newline
    memset(size_buf, 0, sizeof(size_buf));
    while (i < sizeof(size_buf) - 1) {
        if (recv(sock, &size_buf[i], 1, 0) <= 0) {
            printf("Failed to read file size\n");
            return;
        }
        if (size_buf[i] == '\n') {
            size_buf[i] = '\0';
            break;
        }
        i++;
    }
    
    // Check if size starts with "ERR:"
    if (strncmp(size_buf, "ERR:", 4) == 0) {
        printf("Server error: %s\n", size_buf);
        return;
    }
    
    off_t file_size = atol(size_buf);
    if (file_size <= 0) {
        printf("Invalid file size received: %s\n", size_buf);
        return;
    }
    
    printf("Receiving file: %s (%ld bytes)\n", filename, file_size);
    
    // Create file
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("Cannot create file");
        return;
    }
    
    char buffer[MAX_BUFF];
    off_t total_received = 0;
    
    while (total_received < file_size) {
        // Set timeout for each read
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        
        if (select(sock + 1, &readfds, NULL, NULL, &tv) <= 0) {
            printf("\nTimeout waiting for data\n");
            break;
        }
        
        bytes_read = recv(sock, buffer, MIN(MAX_BUFF, file_size - total_received), 0);
        if (bytes_read <= 0) {
            if (bytes_read < 0) perror("Receive error");
            else printf("\nServer closed connection\n");
            break;
        }
        
        ssize_t bytes_written = write(fd, buffer, bytes_read);
        if (bytes_written < 0) {
            perror("Write error");
            break;
        }
        
        total_received += bytes_read;
        
        // Show progress
        float percent = (float)total_received / file_size * 100.0;
        printf("\rProgress: %.1f%% (%ld/%ld bytes)", percent, total_received, file_size);
        fflush(stdout);
    }
    
    close(fd);
    
    if (total_received == file_size) {
        printf("\nDownload complete: %s (%ld bytes)\n", filename, total_received);
    } else {
        printf("\nIncomplete download: %ld/%ld bytes received\n", total_received, file_size);
    }
}

void receive_tar(int sock, char *filetype) {
    // Determine filename based on filetype
    char filename[32];
    switch(filetype[0]) {
        case 'c': strcpy(filename, "c_files.tar"); break;
        case 'p': strcpy(filename, "pdf_files.tar"); break;
        case 't': strcpy(filename, "txt_files.tar"); break;
        case 'z': strcpy(filename, "zip_files.tar"); break;
        default: strcpy(filename, "files.tar");
    }
    
    // Receive the file using the common receive function
    receive_file(sock, filename);
}

void receive_filenames(int sock) {
    char count_buf[32];
    int i = 0;
    
    // Read count character by character until newline
    memset(count_buf, 0, sizeof(count_buf));
    while (i < sizeof(count_buf) - 1) {
        if (recv(sock, &count_buf[i], 1, 0) <= 0) {
            printf("Failed to read file count\n");
            return;
        }
        if (count_buf[i] == '\n') {
            count_buf[i] = '\0';
            break;
        }
        i++;
    }
    
    // Check if count starts with "ERR:"
    if (strncmp(count_buf, "ERR:", 4) == 0) {
        printf("Server error: %s\n", count_buf);
        return;
    }
    
    int file_count = atoi(count_buf);
    printf("Files found: %d\n", file_count);
    
    if (file_count == 0) {
        printf("No files found in the specified path\n");
        return;
    }
    
    // Read each filename
    char buffer[MAX_BUFF];
    printf("\nFile listing:\n");
    printf("-------------------------------------------\n");
    
    for (int j = 0; j < file_count; j++) {
        // Clear buffer
        memset(buffer, 0, MAX_BUFF);
        
        // Read filename character by character until newline
        i = 0;
        while (i < MAX_BUFF - 1) {
            if (recv(sock, &buffer[i], 1, 0) <= 0) {
                printf("Failed to read filename\n");
                return;
            }
            if (buffer[i] == '\n') {
                buffer[i] = '\0';
                break;
            }
            i++;
        }
        
        printf("%s\n", buffer);
    }
    printf("-------------------------------------------\n");
}

void print_help() {
    printf("\nAvailable commands:\n");
    printf("-------------------------------------------\n");
    printf("uploadf <filename> <destination_path>  - Upload a file to server\n");
    printf("downlf <filepath>                     - Download a file from server\n");
    printf("removef <filepath>                    - Remove a file from server\n");
    printf("downltar <filetype>                   - Download all files of specified type as tar\n");
    printf("                                         where filetype is: c, p, t, or z\n");
    printf("dispfnames <pathname>                 - Display filenames in specified path\n");
    printf("help                                  - Show this help message\n");
    printf("exit/quit                             - Exit the client\n");
    printf("-------------------------------------------\n");
    printf("Note: All paths must start with ~S1/\n");
    printf("Example: uploadf myfile.c ~S1/projects/\n");
    printf("Example: downlf ~S1/projects/myfile.c\n");
}

int connect_to_server() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    
    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }
    
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(S1_PORT);
    
    if (inet_pton(AF_INET, S1_IP, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(sock);
        return -1;
    }
    
    // Set connection timeout
    struct timeval tv;
    tv.tv_sec = 3;  // 3 second timeout
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    
    // Connect to S1
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        close(sock);
        return -1;
    }
    
    return sock;
}