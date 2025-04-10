#define _DEFAULT_SOURCE 
#define _XOPEN_SOURCE 700 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ftw.h>
#include <limits.h>

#define PORT 8081
#define MAX_BUFF 4096
#define HOME_DIR "~/S2"

char tar_filepath[PATH_MAX];





// Create directories recursively
void create_dir(char *path) {
    char *p = path;
    while ((p = strchr(p + 1, '/'))) {
        char dir[MAX_BUFF];
        strncpy(dir, path, p - path);
        dir[p - path] = '\0';
        mkdir(dir, 0777);
    }
}

// Function to transform path from S1 notation to S2 notation
char* transform_path(const char* path) {
    static char new_path[MAX_BUFF];
    if (strncmp(path, "~S1/", 4) == 0) {
        snprintf(new_path, sizeof(new_path), "/tmp/S2/%s", path + 4);
    } else {
        strncpy(new_path, path, sizeof(new_path));
        new_path[sizeof(new_path) - 1] = '\0';
    }
    return new_path;
}

// Helper function to expand HOME_DIR to actual path
char* expand_path(const char* path) {
    static char expanded[PATH_MAX];
    
    if (strncmp(path, "~/", 2) == 0) {
        char* home = getenv("TMP");
        if (home) {
            snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
            return expanded;
        }
    }
    
    return (char*)path;
}

// Function to handle file uploading
void handle_upload(int sock, char *filename, char *dest_path, char *file_data, size_t data_size) {
    // Create full path for S2
    char full_path[MAX_BUFF];
     printf("S2: handle_upload - dest_path: %s, filename: %s\n", dest_path, filename);
    
    // Fix this line:
    sprintf(full_path, "/tmp/S2/%s/%s", dest_path, filename);
    
    // Expand path
    //char *expanded_path = expand_path(full_path);
    printf("S2: Expanded path: %s\n", full_path);

    // Create directory structure
    char *dir_path = strdup(full_path);
    char *dir = dirname(dir_path);
    char cmd[MAX_BUFF];

    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);

    printf("S2: Creating directory with cmd: %s\n", cmd);
    system(cmd);
    free(dir_path);
    // printf("\npath : %s\n", expand_path);
    // Create and write to file
    printf("S2: Opening file for writing: %s\n", full_path);
    int fp = open(full_path, O_WRONLY | O_CREAT, 0666);
    if (fp>0) {

        size_t bytes_written = write(fp,file_data,data_size);
        fflush(fp);
        fclose(fp);
        
        if (bytes_written == data_size) {
            printf("S2: Saved file to %s\n", full_path);
            send(sock, "ACK", 3, 0); // Acknowledge success
        } else {
            printf("S2: Error writing file (wrote %zu of %zu bytes)\n", bytes_written, data_size);
            send(sock, "ERR", 3, 0); // Send error
        }
    } else {
        perror("S2: Failed to open file for writing");
        send(sock, "ERR", 3, 0); // Send error
    }
}

// Function to send a file back to S1
void send_file_to_s1(int sock, const char *full_path) {
    struct stat st;
    if (stat(full_path, &st) != 0) {
        send(sock, "ERR", 3, 0);
        return;
    }
    
    // Send file size
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%ld", st.st_size);
    send(sock, size_str, strlen(size_str), 0);
    send(sock, "\n", 1, 0);
    
    // Send file content
    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        perror("S2: Cannot open file for sending");
        return;
    }
    
    char buffer[MAX_BUFF];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, MAX_BUFF)) > 0) {
        send(sock, buffer, bytes_read, 0);
    }
    close(fd);
}

// Function to handle file deletion
void handle_delete(int sock, char *path) {
    // Transform and expand path
    char *transformed = transform_path(path);
    char *expanded = expand_path(transformed);
    
    if (unlink(expanded) == 0) {
        printf("S2: Deleted file %s\n", expanded);
        send(sock, "ACK", 3, 0);
    } else {
        perror("S2: File deletion failed");
        send(sock, "ERR", 3, 0);
    }
}

// Callback function for file traversal when creating tar
int tar_add_file(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    if (typeflag == FTW_F && strstr(fpath, ".pdf") != NULL) {
        char cmd[MAX_BUFF * 2];
        snprintf(cmd, sizeof(cmd), "tar -rf %s '%s'", tar_filepath, fpath);
        system(cmd);
    }
    return 0;
}

// Function to create a tar of all PDF files
void create_pdf_tar(int sock) {
    char *s2_root = expand_path("~/S2");
    
    // Create a temporary tar file
    snprintf(tar_filepath, sizeof(tar_filepath), "%s/pdf.tar", s2_root);
    
    // Create an empty tar file
    char cmd[MAX_BUFF];
    snprintf(cmd, sizeof(cmd), "rm -f %s && touch %s", tar_filepath, tar_filepath);
    system(cmd);
    
    // Traverse the directory and add PDF files to the tar
    nftw(s2_root, tar_add_file, 20, FTW_PHYS);
    
    // Send the tar file to S1
    send_file_to_s1(sock, tar_filepath);
    
    // Clean up
    unlink(tar_filepath);
}

// Function to list all PDF files in a directory
void list_pdf_files(int sock, const char *path) {
    char *transformed = transform_path(path);
    char *expanded = expand_path(transformed);
    
    DIR *dir = opendir(expanded);
    if (!dir) {
        send(sock, "", 0, 0);
        return;
    }
    
    char file_list[MAX_BUFF] = "";
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".pdf") != NULL) {
            strcat(file_list, entry->d_name);
            strcat(file_list, "\n");
        }
    }
    
    closedir(dir);
    send(sock, file_list, strlen(file_list), 0);
}

int main() {

    int serverfd, new_sock;
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    int opt = 1;

    // Create socket
    if ((serverfd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("S2: socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("S2: setsockopt failed");
        exit(EXIT_FAILURE);
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    // Bind and listen
    if (bind(serverfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("S2: bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(serverfd, 5) < 0) {
        perror("S2: listen failed");
        exit(EXIT_FAILURE);
    }

    printf("S2: Listening on port %d...\n", PORT);
     create_dir("/tmp/S2");

    while (1) {
        fflush(stdout);
        // Accept connection from S1
        if ((new_sock = accept(serverfd, (struct sockaddr *)&addr, (socklen_t*)&addrlen)) < 0) {
            perror("S2: accept failed");
            continue;
        }

        // Fork to handle the request
        pid_t pid = fork();
        if (pid == 0) {
            close(serverfd); // Child doesn't need the listener

            char buffer[MAX_BUFF];
            memset(buffer, 0, MAX_BUFF);
            
            // Read command from S1
            ssize_t cmd_len = read(new_sock, buffer, MAX_BUFF);
            if (cmd_len <= 0) {
                close(new_sock);
                exit(0);
            }
            
            buffer[cmd_len] = '\0';
            printf("S2: Received command: %s\n", buffer);
            
            // Parse command
            char *cmd = strtok(buffer, " \n");
            
            if (strcmp(cmd, "uploadf") == 0) {
                // Parse upload command
                char *filename = strtok(NULL, " \n");
                char *dest_path = strtok(NULL, " \n");
                char local_filename[MAX_BUFF];
                char local_dest_path[MAX_BUFF];
                strncpy(local_filename, filename, MAX_BUFF-1);
                local_filename[MAX_BUFF-1] = '\0'; // Ensure null termination
                strncpy(local_dest_path, dest_path, MAX_BUFF-1);
                local_dest_path[MAX_BUFF-1] = '\0'; // Ensure null termination
                
                
                    printf("S2: Parsed uploadf command - filename: '%s', dest_path: '%s'\n", 
               filename ? filename : "NULL", 
               dest_path ? dest_path : "NULL");

                if (!filename || !dest_path) {
                       printf("S2: Missing filename or dest_path in uploadf command\n");
                    send(new_sock, "ERR", 3, 0);
                    close(new_sock);
                    exit(0);
                }
                
                // Read file size
                memset(buffer, 0, MAX_BUFF);
                if (read(new_sock, buffer, MAX_BUFF) <= 0) {
                    close(new_sock);
                    exit(0);
                }
                
                off_t file_size = atol(buffer);
                printf("S2: Expecting file of size: %ld bytes\n", file_size);
                fflush(stdout);
                // strtok's return values might be overwritten later
                

                // Allocate buffer for file data
                char *file_data = (char *)malloc(file_size);
                if (!file_data) {
                    perror("S2: Memory allocation failed");
                    send(new_sock, "ERR", 3, 0);
                    close(new_sock);
                    exit(0);
                }
                
                // Read file data
                size_t total_read = 0;
                ssize_t bytes_read;
                
                while (total_read < file_size && 
                      (bytes_read = recv(new_sock, file_data + total_read, 
                                         file_size - total_read, 0)) > 0) {
                    printf("S2: Received %ld bytes in this read\n", bytes_read);
                    total_read += bytes_read;
                    printf("S2: Total bytes received: %zu of %ld expected\n", total_read, file_size);
                }
                
                if (total_read == file_size) {
                    if (file_data) {
    printf("S2: file_data pointer is not NULL\n");
    // Check first few bytes to see if there's actual content
    if (file_size > 0) {
        printf("S2: First few bytes: %02x %02x %02x %02x\n", 
               file_data[0] & 0xff, 
               file_size > 1 ? file_data[1] & 0xff : 0, 
               file_size > 2 ? file_data[2] & 0xff : 0, 
               file_size > 3 ? file_data[3] & 0xff : 0);
    }
} else {
    printf("S2: WARNING - file_data is NULL!\n");
}
                    handle_upload(new_sock, local_filename, local_dest_path, file_data, file_size);
                } else {
                    send(new_sock, "ERR", 3, 0);
                }
                
                free(file_data);
            } else if (strcmp(cmd, "getf") == 0) {
                // Handle file retrieval for S1
                char *path = strtok(NULL, " \n");
                if (path) {
                    char *transformed = transform_path(path);
                    char *expanded = expand_path(transformed);
                    send_file_to_s1(new_sock, transformed);
                }
            } else if (strcmp(cmd, "removef") == 0) {
                // Handle file deletion
                char *path = strtok(NULL, " \n");
                if (path) {
                    handle_delete(new_sock, path);
                }
            } else if (strcmp(cmd, "tarfiles") == 0) {
                // Create and send tar of all PDF files
                create_pdf_tar(new_sock);
            } else if (strcmp(cmd, "listfiles") == 0) {
                // List PDF files in directory
                char *path = strtok(NULL, " \n");
                if (path) {
                    list_pdf_files(new_sock, path);
                }
            }

            close(new_sock);
            exit(0);
        } else if (pid > 0) {
            close(new_sock); // Parent closes connected socket
            // Clean up any zombie processes
            waitpid(-1, NULL, WNOHANG);
        } else {
            perror("S2: fork failed");
            close(new_sock);
        }
    }

    return 0;
}