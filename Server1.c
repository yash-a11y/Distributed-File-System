#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <ftw.h>
#include <limits.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define PORT 8080
#define MAX_BUFF 4096
#define S2_PORT 8081
#define S3_PORT 8082
#define S4_PORT 8083

// Structure to hold file names for sorting
typedef struct {
    char name[256];
    char type;  // 'c', 'p', 't', 'z' for .c, .pdf, .txt, .zip
} FileEntry;

// Function declarations
void process_client(int client_sock);
ssize_t read_until(int sock, char *buf, char delim);
void mkdirp(const char *path);
void forward_file(char *filename, char *dest_path, int target_port);
void handle_uploadf_command(int client_sock, char *filename, char *dest_path);
void handle_downlf_command(int client_sock, char *filepath);
void handle_removef_command(int client_sock, char *filepath);
void handle_downltar_command(int client_sock, char *filetype);
void handle_dispfnames_command(int client_sock, char *pathname);
char* expand_path(const char* path);
void get_file_from_server(int server_port, char *filepath, int client_sock);
void remove_file_from_server(int server_port, char *filepath);
void get_tar_from_server(int server_port, char *filetype, int client_sock);
void get_filenames_from_server(int server_port, char *pathname, FileEntry *entries, int *count);
int compare_file_entries(const void *a, const void *b);

// Function to read a line up to a delimiter
ssize_t read_until(int sock, char *buf, char delim) {
    ssize_t total = 0;
    while (total < MAX_BUFF - 1) {
        ssize_t n = read(sock, buf + total, 1);
        if (n <= 0) return n; // Return -1 on error, 0 on EOF
        if (buf[total] == delim) break;
        total++;
    }
    buf[total] = '\0';
    return total;
}

// Function to create directories recursively
void mkdirp(const char *path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    int result = system(cmd);
    if (result != 0) {
        perror("mkdir command failed");
    } else {
        printf("Created directory: %s\n", path);
    }
}

// Helper function to expand HOME_DIR to actual path
char* expand_path(const char* path) {
    static char expanded[PATH_MAX];
    
    if (strncmp(path, "~/", 2) == 0) {
        char* home = getenv("HOME");
        if (home) {
            snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
            return expanded;
        }
    }
    
    return (char*)path;
}

// Function to forward a file to another server (S2, S3, or S4)
void forward_file(char *filename, char *dest_path, int target_port) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Forward socket creation failed");
        return;
    }
    
    struct sockaddr_in server_addr;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(target_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("S1: Failed to connect to storage server");
        close(server_sock);
        return;
    }

    // Send command to storage server
    char command[MAX_BUFF];
    snprintf(command, sizeof(command), "uploadf %s %s\n", filename, dest_path);

    write(server_sock, command, strlen(command));
    
    // Send file content
    char file_path[MAX_BUFF];
    snprintf(file_path, sizeof(file_path), "/tmp/S1/%s", dest_path);
    // Create directory if it doesn't exist
    mkdirp(file_path);
    // Now build the full path with filename
    snprintf(file_path, sizeof(file_path), "/tmp/S1/%s/%s", dest_path, filename);
    char *expanded_path = expand_path(file_path);
    printf("Forward file - full path: %s\n", expanded_path); // Debug output

    struct stat st;
    if (stat(expanded_path, &st) != 0) {
        perror("Cannot stat file for forwarding");
        close(server_sock);
        return;
    }
    
    // Send file size
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%ld", st.st_size);
    write(server_sock, size_str, strlen(size_str));
    
    int fd = open(expanded_path, O_RDONLY);
    if (fd < 0) {
        perror("Cannot open file for forwarding");
        close(server_sock);
        return;
    }
    
    char buffer[MAX_BUFF];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, MAX_BUFF)) > 0) {
        write(server_sock, buffer, bytes_read);
    }
    close(fd);
    
    // Wait for acknowledgment
    memset(buffer, 0, MAX_BUFF);
    read(server_sock, buffer, 3);
    
    if (strcmp(buffer, "ACK") == 0) {
        printf("File successfully forwarded to server on port %d\n", target_port);
        printf("Deleting local file: %s\n", expanded_path); // Add this line
   
        // Delete local copy of forwarded file
        if (unlink(expanded_path) == 0) {
            printf("Deleted local copy of forwarded file: %s\n", expanded_path);
        } else {
            perror("Failed to delete forwarded file");
        }
    } else {
        printf("Error: Server rejected file\n");
    }
    
    close(server_sock);
}

// Function to handle uploadf command
void handle_uploadf_command(int client_sock, char *filename, char *dest_path) {
    // Read file size
    char size_header[32];
    ssize_t bytes_read = read_until(client_sock, size_header, '\n');
    if (bytes_read <= 0) {
        write(client_sock, "ERR: Missing file size\n", 22);
        return;
    }
    
    off_t file_size = atol(size_header);
    if (file_size <= 0) {
        write(client_sock, "ERR: Invalid file size\n", 22);
        return;
    }
    
    // Make sure dest_path starts with ~S1/
    if (strncmp(dest_path, "~S1/", 4) != 0) {
        write(client_sock, "ERR: Path must start with ~S1/\n", 29);
        return;
    }
    
    // Convert path (e.g., ~S1/f1 -> /tmp/S1/f1/xyz.c)
    char full_path[MAX_BUFF];
    snprintf(full_path, sizeof(full_path), "/tmp/S1/%s/%s", 
             dest_path + 4, filename); // Skip ~S1/
    
    // Create directory structure
    char *dir_path = strdup(full_path);
    char *dir = dirname(dir_path);
    mkdirp(dir);
    free(dir_path);
    
    // Save file temporarily
    int fd = open(full_path, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) {
        perror("File creation failed");
        write(client_sock, "ERR: File creation failed\n", 26);
        return;
    }
    
    off_t remaining = file_size;
    off_t total_written = 0;
    char buffer[MAX_BUFF];
    
    printf("Receiving file of size: %ld bytes\n", file_size);
    
    while (remaining > 0) {
        bytes_read = read(client_sock, buffer, MIN(MAX_BUFF, remaining));
        if (bytes_read <= 0) break;
        
        ssize_t bytes_written = write(fd, buffer, bytes_read);
        if (bytes_written < 0) {
            perror("Write error");
            break;
        }
        
        total_written += bytes_written;
        remaining -= bytes_read;
        
        printf("Received: %ld bytes, Remaining: %ld bytes\n", bytes_read, remaining);
    }
    close(fd);
    
    if (remaining > 0) {
        write(client_sock, "ERR: Incomplete file transfer\n", 30);
        unlink(full_path);
        return;
    }
    
    // Forward to appropriate server based on file extension
    char *ext = strrchr(filename, '.');
    if (ext) {
        int target_port = 0;
        if (strcmp(ext, ".pdf") == 0) target_port = S2_PORT;
        else if (strcmp(ext, ".txt") == 0) target_port = S3_PORT;
        else if (strcmp(ext, ".zip") == 0) target_port = S4_PORT;
        
        if (target_port) {
            forward_file(filename, dest_path + 4, target_port);
            write(client_sock, "OK: File stored remotely\n", 25);
            unlink(full_path);
        } else if (strcmp(ext, ".c") == 0) {
            // Keep .c files locally
            write(client_sock, "OK: File stored locally\n", 23);
        } else {
            write(client_sock, "ERR: Unsupported file type\n", 27);
            unlink(full_path);
        }
    } else {
        write(client_sock, "ERR: File has no extension\n", 27);
        unlink(full_path);
    }
}

// Function to get a file from another server (S2, S3, or S4)
void get_file_from_server(int server_port, char *filepath, int client_sock) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        write(client_sock, "ERR: Cannot connect to storage server\n", 38);
        return;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to connect to storage server");
        write(client_sock, "ERR: Cannot connect to storage server\n", 38);
        close(server_sock);
        return;
    }
    
    // Send getf command to the server
    char command[MAX_BUFF];
    snprintf(command, sizeof(command), "getf %s", filepath);
    write(server_sock, command, strlen(command));
    
    // Read file size
    char size_buf[32];
    read_until(server_sock, size_buf, '\n');
    off_t file_size = atol(size_buf);
    
    // Forward file size to client
    char size_header[64];
    snprintf(size_header, sizeof(size_header), "%ld\n", file_size);
    write(client_sock, size_header, strlen(size_header));
    
    // Forward file data from server to client
    char buffer[MAX_BUFF];
    ssize_t bytes_read;
    off_t total_read = 0;
    
    while (total_read < file_size && 
          (bytes_read = read(server_sock, buffer, MIN(MAX_BUFF, file_size - total_read))) > 0) {
        write(client_sock, buffer, bytes_read);
        total_read += bytes_read;
    }
    
    close(server_sock);
}

// Function to handle downlf command
void handle_downlf_command(int client_sock, char *filepath) {
    // Make sure filepath starts with ~S1/
    if (strncmp(filepath, "~S1/", 4) != 0) {
        write(client_sock, "ERR: Path must start with ~S1/\n", 29);
        return;
    }
    
    // Extract filename from path
    char *filename = strrchr(filepath, '/');
    if (!filename) {
        write(client_sock, "ERR: Invalid file path\n", 22);
        return;
    }
    filename++; // Skip the '/'
    
    // Check file extension
    char *ext = strrchr(filename, '.');
    if (!ext) {
        write(client_sock, "ERR: File has no extension\n", 27);
        return;
    }
    
    if (strcmp(ext, ".c") == 0) {
        // C files are stored locally
        char full_path[MAX_BUFF];
        snprintf(full_path, sizeof(full_path), "/tmp/S1/%s", filepath + 4); // Skip ~S1/
        
        struct stat st;
        if (stat(full_path, &st) != 0) {
            write(client_sock, "ERR: File not found\n", 20);
            return;
        }
        
        // Send file size
        char size_header[64];
        snprintf(size_header, sizeof(size_header), "%ld\n", st.st_size);
        write(client_sock, size_header, strlen(size_header));
        
        // Send file content
        int fd = open(full_path, O_RDONLY);
        if (fd < 0) {
            perror("Cannot open file");
            return;
        }
        
        char buffer[MAX_BUFF];
        ssize_t bytes_read;
        while ((bytes_read = read(fd, buffer, MAX_BUFF)) > 0) {
            write(client_sock, buffer, bytes_read);
        }
        close(fd);
    } else if (strcmp(ext, ".pdf") == 0) {
        // PDF files are stored on S2
        get_file_from_server(S2_PORT, filepath, client_sock);
    } else if (strcmp(ext, ".txt") == 0) {
        // TXT files are stored on S3
        get_file_from_server(S3_PORT, filepath, client_sock);
    } else if (strcmp(ext, ".zip") == 0) {
        // ZIP files are stored on S4
        get_file_from_server(S4_PORT, filepath, client_sock);
    } else {
        write(client_sock, "ERR: Unsupported file type\n", 27);
    }
}

// Function to remove a file from another server
void remove_file_from_server(int server_port, char *filepath) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        return;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to connect to storage server");
        close(server_sock);
        return;
    }
    
    // Send removef command to the server
    char command[MAX_BUFF];
    snprintf(command, sizeof(command), "removef %s", filepath);
    write(server_sock, command, strlen(command));
    
    // Wait for acknowledgment
    char response[4];
    read(server_sock, response, 3);
    response[3] = '\0';
    
    close(server_sock);
}

// Function to handle removef command
void handle_removef_command(int client_sock, char *filepath) {
    // Make sure filepath starts with ~S1/
    if (strncmp(filepath, "~S1/", 4) != 0) {
        write(client_sock, "ERR: Path must start with ~S1/\n", 29);
        return;
    }
    
    // Extract filename from path
    char *filename = strrchr(filepath, '/');
    if (!filename) {
        write(client_sock, "ERR: Invalid file path\n", 22);
        return;
    }
    filename++; // Skip the '/'
    
    // Check file extension
    char *ext = strrchr(filename, '.');
    if (!ext) {
        write(client_sock, "ERR: File has no extension\n", 27);
        return;
    }
    
    // Continuation of handle_removef_command function where it was cut off
    if (strcmp(ext, ".c") == 0) {
        // C files are stored locally
        char full_path[MAX_BUFF];
        snprintf(full_path, sizeof(full_path), "/tmp/S1/%s", filepath + 4); // Skip ~S1/
        
        // Try to remove the file
        if (unlink(full_path) == 0) {
            write(client_sock, "OK: File removed\n", 17);
        } else {
            write(client_sock, "ERR: Could not remove file\n", 27);
            perror("File removal failed");
        }
    } else if (strcmp(ext, ".pdf") == 0) {
        // PDF files are stored on S2
        remove_file_from_server(S2_PORT, filepath);
        write(client_sock, "OK: File removed from S2\n", 24);
    } else if (strcmp(ext, ".txt") == 0) {
        // TXT files are stored on S3
        remove_file_from_server(S3_PORT, filepath);
        write(client_sock, "OK: File removed from S3\n", 24);
    } else if (strcmp(ext, ".zip") == 0) {
        // ZIP files are stored on S4
        remove_file_from_server(S4_PORT, filepath);
        write(client_sock, "OK: File removed from S4\n", 24);
    } else {
        write(client_sock, "ERR: Unsupported file type\n", 27);
    }
}

// Function to get tar file from server
void get_tar_from_server(int server_port, char *filetype, int client_sock) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        write(client_sock, "ERR: Cannot connect to storage server\n", 38);
        return;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to connect to storage server");
        write(client_sock, "ERR: Cannot connect to storage server\n", 38);
        close(server_sock);
        return;
    }
    
    // Send get_tar command to the server
    char command[MAX_BUFF];
    snprintf(command, sizeof(command), "gettar %s", filetype);
    write(server_sock, command, strlen(command));
    
    // Read file size
    char size_buf[32];
    read_until(server_sock, size_buf, '\n');
    off_t file_size = atol(size_buf);
    
    // Forward file size to client
    char size_header[64];
    snprintf(size_header, sizeof(size_header), "%ld\n", file_size);
    write(client_sock, size_header, strlen(size_header));
    
    // Forward tar file data from server to client
    char buffer[MAX_BUFF];
    ssize_t bytes_read;
    off_t total_read = 0;
    
    while (total_read < file_size && 
          (bytes_read = read(server_sock, buffer, MIN(MAX_BUFF, file_size - total_read))) > 0) {
        write(client_sock, buffer, bytes_read);
        total_read += bytes_read;
    }
    
    close(server_sock);
}

// Function to handle downltar command
void handle_downltar_command(int client_sock, char *filetype) {
    // Check valid file types
    if (strcmp(filetype, "c") == 0) {
        // Create a temporary directory for creating the tar
        char temp_dir[MAX_BUFF];
        snprintf(temp_dir, sizeof(temp_dir), "/tmp/S1_tar_%d", getpid());
        mkdirp(temp_dir);
        
        // Create a tar file of all .c files
        char tar_cmd[MAX_BUFF];
        snprintf(tar_cmd, sizeof(tar_cmd), 
                 "find /tmp/S1 -name '*.c' -exec tar -rf %s/c_files.tar {} \\; 2>/dev/null || true", 
                 temp_dir);
        system(tar_cmd);
        
        // Get the size of the tar file
        char tar_path[MAX_BUFF];
        snprintf(tar_path, sizeof(tar_path), "%s/c_files.tar", temp_dir);
        
        struct stat st;
        if (stat(tar_path, &st) != 0 || st.st_size == 0) {
            write(client_sock, "0\n", 2); // No files or empty tar
            rmdir(temp_dir);
            return;
        }
        
        // Send file size
        char size_header[64];
        snprintf(size_header, sizeof(size_header), "%ld\n", st.st_size);
        write(client_sock, size_header, strlen(size_header));
        
        // Send file content
        int fd = open(tar_path, O_RDONLY);
        if (fd < 0) {
            perror("Cannot open tar file");
            return;
        }
        
        char buffer[MAX_BUFF];
        ssize_t bytes_read;
        while ((bytes_read = read(fd, buffer, MAX_BUFF)) > 0) {
            write(client_sock, buffer, bytes_read);
        }
        close(fd);
        
        // Clean up
        unlink(tar_path);
        rmdir(temp_dir);
    } else if (strcmp(filetype, "p") == 0) {
        // PDF files are stored on S2
        get_tar_from_server(S2_PORT, filetype, client_sock);
    } else if (strcmp(filetype, "t") == 0) {
        // TXT files are stored on S3
        get_tar_from_server(S3_PORT, filetype, client_sock);
    } else if (strcmp(filetype, "z") == 0) {
        // ZIP files are stored on S4
        get_tar_from_server(S4_PORT, filetype, client_sock);
    } else {
        write(client_sock, "ERR: Unsupported file type\n", 27);
    }
}

// Function to get filenames from a server
void get_filenames_from_server(int server_port, char *pathname, FileEntry *entries, int *count) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        return;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to connect to storage server");
        close(server_sock);
        return;
    }
    
    // Send listf command to the server
    char command[MAX_BUFF];
    snprintf(command, sizeof(command), "listf %s", pathname);
    write(server_sock, command, strlen(command));
    
    // Read count of files
    char count_buf[32];
    read_until(server_sock, count_buf, '\n');
    int file_count = atoi(count_buf);
    
    // Read filenames
    char buffer[MAX_BUFF];
    for (int i = 0; i < file_count && *count < 1000; i++) {
        read_until(server_sock, buffer, '\n');
        
        // Add to entries array with appropriate type
        if (server_port == S2_PORT) {
            strncpy(entries[*count].name, buffer, sizeof(entries[*count].name) - 1);
            entries[*count].type = 'p';
            (*count)++;
        } else if (server_port == S3_PORT) {
            strncpy(entries[*count].name, buffer, sizeof(entries[*count].name) - 1);
            entries[*count].type = 't';
            (*count)++;
        } else if (server_port == S4_PORT) {
            strncpy(entries[*count].name, buffer, sizeof(entries[*count].name) - 1);
            entries[*count].type = 'z';
            (*count)++;
        }
    }
    
    close(server_sock);
}

// Compare function for sorting files
int compare_file_entries(const void *a, const void *b) {
    return strcmp(((const FileEntry *)a)->name, ((const FileEntry *)b)->name);
}

// Function to handle dispfnames command
void handle_dispfnames_command(int client_sock, char *pathname) {
    // Make sure pathname starts with ~S1/
    if (strncmp(pathname, "~S1/", 4) != 0) {
        write(client_sock, "ERR: Path must start with ~S1/\n", 29);
        return;
    }
    
    // Convert pathname
    char dir_path[MAX_BUFF];
    snprintf(dir_path, sizeof(dir_path), "/tmp/S1/%s", pathname + 4); // Skip ~S1/
    
    // Array to hold file entries
    FileEntry entries[1000];
    int count = 0;
    
    // List local C files
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) && count < 1000) {
            // Skip . and ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
                
            // Check if it's a .c file
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".c") == 0) {
                strncpy(entries[count].name, entry->d_name, sizeof(entries[count].name) - 1);
                entries[count].type = 'c';
                count++;
            }
        }
        closedir(dir);
    }
    
    // Get filenames from other servers
    get_filenames_from_server(S2_PORT, pathname, entries, &count); // PDF files
    get_filenames_from_server(S3_PORT, pathname, entries, &count); // TXT files
    get_filenames_from_server(S4_PORT, pathname, entries, &count); // ZIP files
    
    // Sort entries alphabetically
    qsort(entries, count, sizeof(FileEntry), compare_file_entries);
    
    // Send the count to client
    char count_header[32];
    snprintf(count_header, sizeof(count_header), "%d\n", count);
    write(client_sock, count_header, strlen(count_header));
    
    // Send the file list to client
    for (int i = 0; i < count; i++) {
        char file_info[MAX_BUFF];
        // Format: filename (type)
        char type_char = entries[i].type;
        char *type_name = "";
        
        switch (type_char) {
            case 'c': type_name = "C source"; break;
            case 'p': type_name = "PDF document"; break;
            case 't': type_name = "Text file"; break;
            case 'z': type_name = "ZIP archive"; break;
        }
        
        snprintf(file_info, sizeof(file_info), "%s (%s)\n", entries[i].name, type_name);
        write(client_sock, file_info, strlen(file_info));
    }
}

// Function to process client requests
void process_client(int client_sock) {
    char buffer[MAX_BUFF];
    
    // Read command
    ssize_t bytes_read = read_until(client_sock, buffer, ' ');
    if (bytes_read <= 0) {
        close(client_sock);
        return;
    }
    buffer[bytes_read] = '\0';
    
    if (strcmp(buffer, "uploadf") == 0) {
        // Read filename
        memset(buffer, 0, MAX_BUFF);
        bytes_read = read_until(client_sock, buffer, ' ');
        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }
        buffer[bytes_read] = '\0';
        char filename[MAX_BUFF];
        strncpy(filename, buffer, MAX_BUFF - 1);
        
        // Read destination path
        memset(buffer, 0, MAX_BUFF);
        bytes_read = read_until(client_sock, buffer, '\n');
        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }
        buffer[bytes_read] = '\0';
        char dest_path[MAX_BUFF];
        strncpy(dest_path, buffer, MAX_BUFF - 1);
        
        handle_uploadf_command(client_sock, filename, dest_path);
    } else if (strcmp(buffer, "downlf") == 0) {
        // Read filepath
        memset(buffer, 0, MAX_BUFF);
        bytes_read = read_until(client_sock, buffer, '\n');
        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }
        buffer[bytes_read] = '\0';
        
        handle_downlf_command(client_sock, buffer);
    } else if (strcmp(buffer, "removef") == 0) {
        // Read filepath
        memset(buffer, 0, MAX_BUFF);
        bytes_read = read_until(client_sock, buffer, '\n');
        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }
        buffer[bytes_read] = '\0';
        
        handle_removef_command(client_sock, buffer);
    } else if (strcmp(buffer, "downltar") == 0) {
        // Read filetype
        memset(buffer, 0, MAX_BUFF);
        bytes_read = read_until(client_sock, buffer, '\n');
        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }
        buffer[bytes_read] = '\0';
        
        handle_downltar_command(client_sock, buffer);
    } else if (strcmp(buffer, "dispfnames") == 0) {
        // Read pathname
        memset(buffer, 0, MAX_BUFF);
        bytes_read = read_until(client_sock, buffer, '\n');
        if (bytes_read <= 0) {
            close(client_sock);
            return;
        }
        buffer[bytes_read] = '\0';
        
        handle_dispfnames_command(client_sock, buffer);
    } else {
        write(client_sock, "ERR: Unknown command\n", 21);
    }
    
    close(client_sock);
}

int main() {
    int server_fd, client_sock;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;
    
    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // Bind socket
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("S1 Server started on port %d\n", PORT);
    
    // Create base directory
    mkdirp("/tmp/S1");
    
    // Main server loop
    while (1) {
        if ((client_sock = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            continue;
        }
        
        printf("New client connected\n");
        
        // Fork a child process to handle the client
        pid_t pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            close(client_sock);
            continue;
        } else if (pid == 0) {
            // Child process
            close(server_fd);
            process_client(client_sock);
            exit(EXIT_SUCCESS);
        } else {
            // Parent process
            close(client_sock);
            // Clean up zombie processes
            waitpid(-1, NULL, WNOHANG);
        }
    }
    
    return 0;
}