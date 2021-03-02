#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "threadpool.h"

#define OK 200
#define FOUND 302
#define BAD_REQUEST 400
#define FORBIDDEN 403
#define NOT_FOUND 404
#define INTERNAL_SERVER_ERROR 500
#define NOT_SUPPORTED 501

#define DIR_CONTENT 102
#define RETURN_FILE 103

#define REQ_MAX_SIZE 4000
#define MIN_RES_SIZE 300

#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
#define FOUND_RESPONSE_TAMPLATE "HTTP/1.1 302 Found\r\nServer: webserver/1.0\r\nDate: %s\r\nLocation: %s/\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n<HTML><HEAD><TITLE>302 Found</TITLE></HEAD><BODY><H4>302 Found</H4>Directories must end with a slash.</BODY></HTML>"
#define ERROR_RESPONSE_TAMPLATE "HTTP/1.1 %s\r\nServer: webserver/1.0\r\nDate: %s\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n<HTML><HEAD><TITLE>%s</TITLE></HEAD><BODY><H4>%s</H4>%s</BODY></HTML>"
#define FILE_RESPONSE_TAMPLATE "HTTP/1.1 200 OK\r\nServer: webserver/1.0\r\nDate: %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n"
#define DIR_HEADERS_TAMPLATE "HTTP/1.1 200 OK\r\nServer: webserver/1.0\r\nDate: %s\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n"             // 130
#define DIR_TABLE_HEAD_TEMPLATE "<HTML>\n<HEAD><TITLE>Index of %s</TITLE></HEAD>\n<BODY>\n<H4>Index of %s</H4>\n<table CELLSPACING=8>\n<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n" // 152
#define DIR_ENTERY_TAMPLATE "<tr>\n<td><A HREF=\"%s\">%s</A></td>\n<td>%s</td>\n<td>%s</td>\n</tr>"                                                                                                 // 55                                                                                                                                                                                                                                      // 53
#define DIR_TABLE_END_TEMPLATE "</table>\n<HR>\n<ADDRESS>webserver/1.0</ADDRESS>\n</BODY>\n</HTML>"                                                                                                 // 62

void usage()
{
    printf("Usage: server <port> <pool-size>\n");
}

size_t log_10(size_t x)
{
    int i = 0;
    while (x / 10 != 0)
    {
        x = x / 10;
        i++;
    }
    return i;
}

// function looking for \r\n and put '
int find_end_line(char *buff)
{
    int bn = 0;
    for (int i = 0; i < REQ_MAX_SIZE; i++)
    {
        if (bn == 1)
        {
            if (buff[i] == '\n')
            {
                buff[i - 1] = '\0';
                return 1;
            }
            else
                bn = 0;
        }
        else if (buff[i] == '\r')
            bn = 1;
    }
    return 0;
}

// function to check if port is match dev demand
int validatePort(char *port)
{
    int vld = atoi(port);
    if (strlen(port) != log_10(vld) + 1)
        return 0;
    if (vld <= 0 || vld > 65535) //check if x > 1024 is required
        return 0;
    return 1;
}

//function to check permission to path, return: 0 - granted, 302 - fobidden, 500 - sys call
int check_permission(char *path)
{
    // create another empty string to add dir by dir for checking permissions
    int path_len = strlen(path);
    char part[path_len + 1];
    memset(part, '\0', path_len + 1);
    char *ptr = path;
    // idx of first '/'
    int idx = 0;
    do
    {
        ptr = strchr(&ptr[1], '/');
        if (!ptr)
            idx = path_len - 1;
        else
            idx = (int)(ptr - path);
        // copy till current dir to check
        strncpy(part, path, idx + 1);
        struct stat fs;
        if (stat(part, &fs) == -1)
            return INTERNAL_SERVER_ERROR;
        // in case path pointed dir check for EXEC permissions for OTHER
        if (S_ISDIR(fs.st_mode))
            if (!(S_IXOTH & fs.st_mode))
                return FORBIDDEN;
        // in case path pointed to reg file check READ permissions for OTHER
        if (S_ISREG(fs.st_mode))
        {
            if (!(S_IROTH & fs.st_mode))
                return FORBIDDEN;
            else
                return 0;
        }
    } while (idx < path_len - 1);
    return 0;
}

// analyse function return a response code.
// in other case then BAD_REQUEST or NOT_SUPPORTED, request pointer will overwrite to be path
int analyse(char *request)
{

    if (!find_end_line(request))
        return BAD_REQUEST;

    // cut line - put a NULL at the end of the line
    // validate path not contains a "//"
    if (strstr(request, "//"))
        return BAD_REQUEST;
    // set pointers to required arguments: method, path, protocol
    char *method, *path, *protocol = NULL;
    method = strtok(request, " ");
    path = strtok(NULL, " ");
    protocol = strtok(NULL, "\0");
    // validate pointers succesfully created, also validtae http protocol is known
    if (!method || !path || !protocol || (strncmp(protocol, "HTTP/1.1", 8) != 0 && strncmp(protocol, "HTTP/1.0", 8) != 0))
        return BAD_REQUEST;
    // validtae method support
    if (strcmp(method, "GET") != 0)
        return NOT_SUPPORTED;
    // creating a proper path that starts with "."
    int proper_path_len = strlen(path) + 2;
    char proper_path[proper_path_len];
    sprintf(proper_path, ".%s", path);
    // request -> path
    sprintf(request, "%s", proper_path);
    // validate path integrity
    if (access(proper_path, F_OK) != 0)
        return NOT_FOUND;
    // check_permissions return 0 if permission granted to data
    int permission = check_permission(proper_path);
    if (permission > 0)
        return permission;
    // get file stat for file information, (stat is sys call so in case of failure return 500 ERROR)
    struct stat fs;
    if (stat(proper_path, &fs) == -1)
        return INTERNAL_SERVER_ERROR;
    if (S_ISREG(fs.st_mode))
        return RETURN_FILE;
    if (S_ISDIR(fs.st_mode))
    {
        if (proper_path[proper_path_len - 2] != '/')
            return FOUND;
        char index_path[proper_path_len + 10];
        sprintf(index_path, "%sindex.html", proper_path);
        if (access(index_path, F_OK) != 0)
            return DIR_CONTENT;
        // check permission to index.html
        permission = check_permission(index_path);
        if (permission > 0)
            return permission;
        sprintf(request, "%s", index_path);
        return RETURN_FILE;
    }
    return FORBIDDEN;
}

int send_error(int type, int fd, char *now)
{
    const int SIZE = 30;
    char response_type[SIZE];
    char response_body[SIZE];
    char response[300];
    switch (type)
    {
    case BAD_REQUEST:
        strncpy(response_type, "400 Bad Request", SIZE);
        strncpy(response_body, "Bad Request.", SIZE);
        break;
    case FORBIDDEN:
        strncpy(response_type, "403 Forbidden", SIZE);
        strncpy(response_body, "Access Denied.", SIZE);
        break;
    case NOT_FOUND:
        strncpy(response_type, "404 Not Found", SIZE);
        strncpy(response_body, "File not found.", SIZE);
        break;
    case INTERNAL_SERVER_ERROR:
        strncpy(response_type, "500 Internal Server Error", SIZE);
        strncpy(response_body, "Some server side error.", SIZE);
        break;
    case NOT_SUPPORTED:
        strncpy(response_type, "501 Not supported", SIZE);
        strncpy(response_body, "Method is not supported.", SIZE);
        break;
    }
    // 63 - num of all html tags ONLY in error response
    int content_length = 63 + 2 * strlen(response_type) + strlen(response_body);
    sprintf(response, ERROR_RESPONSE_TAMPLATE,
            response_type, now, content_length, response_type, response_type, response_body);
    int response_length = strlen(response);
    int writed = write(fd, response, response_length);
    if (writed != response_length)
        perror("error: write headers to fd failed - (send_error)");
    // printf("----- writed: %d ------\n", writed);
    return 0;
}
int send_found(char *path, int fd, char *now)
{
    char response[MIN_RES_SIZE + strlen(path)];
    int content_length = 115;
    sprintf(response, FOUND_RESPONSE_TAMPLATE, now, &path[1], content_length);
    int response_length = strlen(response);
    int writed = write(fd, response, response_length);
    if (writed != response_length)
        perror("error: write headers to fd failed - (send_found)");
    return 0;
}

//return str contains type
char *get_mime_type(char *name)
{
    char *ext = strrchr(name, '.');
    if (!ext)
        return NULL;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".au") == 0)
        return "audio/basic";
    if (strcmp(ext, ".wav") == 0)
        return "audio/wav";
    if (strcmp(ext, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0)
        return "video/mpeg";
    if (strcmp(ext, ".mp3") == 0)
        return "audio/mpeg";
    return NULL;
}

// function to send file response to client. received: path to dile, file descriptor, corrent time (char *).
int send_file(char *path, int fd, char *now)
{
    struct stat fs;
    // gets file stat
    if (stat(path, &fs) == -1)
        return send_error(INTERNAL_SERVER_ERROR, fd, now);
    // get last modified time from stat into char * buffer
    char timebuf_last_mod[128];
    strftime(timebuf_last_mod, sizeof(timebuf_last_mod), RFC1123FMT, gmtime(&fs.st_mtime));
    // get the relevant content type (text/html / imj ...)
    char headers[256];
    char *content_type = get_mime_type(path);
    // open stream to file
    FILE *file = fopen(path, "r");
    if (!file)
        return send_error(INTERNAL_SERVER_ERROR, fd, now);

    // create response headers
    sprintf(headers, FILE_RESPONSE_TAMPLATE, now, content_type, fs.st_size);
    int headers_length = strlen(headers);
    // write the headers into file descriptor
    int writed = write(fd, headers, headers_length);
    if (writed != headers_length)
    {
        perror("error: write headers to fd failed - (send_file)");
        if (fclose(file) < 0)
            perror("ERROR: close file failed - leak.");
        return 0;
    }
    // buff to hold file
    unsigned char *file_buff[1000];
    int readed = 0;
    long int tot_readed = 0;
    long int tot_writed = 0;

    do
    {
        // read from stream into buffer
        readed = fread(file_buff, 1, 1000, file);
        // write from buffer into file descriptor
        writed = write(fd, file_buff, readed);
        tot_readed += readed;
        tot_writed += writed;

        // repeat until the end of the file
    } while (readed > 0 && readed == writed);
    // writed += write(fd, "\r\n", 2);

    // printf("file size: %ld, readed: %ld, writed: %ld\n", fs.st_size, tot_readed, tot_writed);
    if (tot_writed != tot_readed)
        perror("ERROR: write file to fd failed.");
    if (fclose(file) < 0)
        perror("ERROR: close file failed - leak.");
    return 0;
}

int send_dir_content(char *path, int fd, char *now)
{
    // create ref to directory
    DIR *dir = opendir(path);
    if (!dir)
        return send_error(INTERNAL_SERVER_ERROR, fd, now);
    // get directory stat
    struct stat fs;
    if (stat(path, &fs) == -1)
    {
        if (closedir(dir) == -1)
            perror("ERROR: close directory failed");
        return send_error(INTERNAL_SERVER_ERROR, fd, now);
    }
    // str to handle last modified time's
    char timebuf_last_mod[128];
    strftime(timebuf_last_mod, sizeof(timebuf_last_mod), RFC1123FMT, gmtime(&fs.st_mtime));
    // counting enteries in directory
    int num_of_enteries = 0;
    // ref's to entery template arguments
    int date_length = strlen(timebuf_last_mod);
    // int dir_template_length = 53;
    int dir_path_len = strlen(path);
    // counter to detect content total size
    long int content_length = strlen(DIR_TABLE_HEAD_TEMPLATE) + strlen(DIR_TABLE_END_TEMPLATE) + (2 * dir_path_len) + 1;
    // get entery ref
    struct dirent *dir_entry;
    // counting content size by adding each entery full template size (long int for file size)
    const int entery_basic_length = strlen(DIR_ENTERY_TAMPLATE) + dir_path_len + date_length + sizeof(long) + 1;
    while ((dir_entry = readdir(dir)) != NULL)
    {
        content_length = content_length + entery_basic_length + (2 * strlen(dir_entry->d_name));
        num_of_enteries++;
    }
    // get headers length
    int headers_length = strlen(DIR_HEADERS_TAMPLATE) + log_10(content_length) + (2 * date_length) + 2; // 2 = 1 for log10+1 (length of num), 1 for '\0' /--/ 2 times: for last mode & date
    char headers[headers_length];

    // str to handle content itself
    char content[content_length];
    sprintf(content, DIR_TABLE_HEAD_TEMPLATE, path, path);
    // pointer where to write each loop
    int write_to_here = strlen(content);
    rewinddir(dir);
    for (int i = 0; i < num_of_enteries; i++)
    {
        dir_entry = readdir(dir);
        int d_name_length = strlen(dir_entry->d_name) + 2; // 2 = 1 reserve for dir, 1 for '\0'
        // entery name for show
        char d_name[d_name_length];
        // entery name for href
        sprintf(d_name, "%s", dir_entry->d_name);
        int full_path_length = dir_path_len + d_name_length;
        char entery_full_path[full_path_length];
        // create a valid path to file
        sprintf(entery_full_path, "%s%s", path, d_name);
        // get stat to sepecific file
        if (stat(entery_full_path, &fs) == -1)
        {
            printf("ERROR: stat failed - dir content");
            continue;
        }
        // get entery last modified date
        strftime(timebuf_last_mod, sizeof(timebuf_last_mod), RFC1123FMT, gmtime(&fs.st_mtime));
        // create a str to handle entery size (if reg file)
        int entery_size_length = log_10(fs.st_size) + 2; // 2 = 1 for actually length, 1 for '\0'
        char entery_size[entery_size_length];
        if (S_ISREG(fs.st_mode))
        {
            sprintf(entery_size, "%ld", fs.st_size);
        }
        else
        {
            entery_size[0] = '\0';
            sprintf(&d_name[d_name_length - 2], "/");
        }
        sprintf(&content[write_to_here], DIR_ENTERY_TAMPLATE, dir_entry->d_name, dir_entry->d_name, timebuf_last_mod, entery_size);
        write_to_here = write_to_here + strlen(&content[write_to_here]);
    }
    sprintf(&content[write_to_here], DIR_TABLE_END_TEMPLATE);
    content_length = strlen(content);
    //create headers
    sprintf(headers, DIR_HEADERS_TAMPLATE, now, content_length, timebuf_last_mod);
    int writed = write(fd, headers, strlen(headers));
    // write headers to file descriptor
    if (writed != strlen(headers))
    {
        perror("error: write headers to fd failed - (send_dir_content)");
        if (closedir(dir) == -1)
            perror("ERROR: close directory failed");
        return 0;
    }
    // write content to file descriptor
    writed = write(fd, content, content_length);
    if (writed != content_length)
        perror("ERROR: write content to fd failed - (send_dir_content)");
    if (closedir(dir) == -1)
        perror("ERROR: close directory failed");

    return 0;
}

// dispatch function for thread from threadpool
int handle_client(void *fd_buff)
{
    int fd = atoi((char *)fd_buff);
    char buff[REQ_MAX_SIZE + 1];
    buff[REQ_MAX_SIZE] = '\0';
    int request_size = read(fd, buff, REQ_MAX_SIZE);
    if (request_size < 0)
    {
        perror("ERROR: read failure");
    }
    else if (request_size == 0)
    {
        // client disconnected
    }
    else
    {
        time_t now;
        char timebuf_now[128];
        now = time(NULL);
        strftime(timebuf_now, sizeof(timebuf_now), RFC1123FMT, gmtime(&now));
        // get response code
        int result = analyse(buff);
        switch (result)
        {
        case RETURN_FILE:
            send_file(buff, fd, timebuf_now);
            break;
        case DIR_CONTENT:
            send_dir_content(buff, fd, timebuf_now);
            break;
        case FOUND:
            send_found(buff, fd, timebuf_now);
            break;
        default:
            send_error(result, fd, timebuf_now);
            break;
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    // validate number of args received equals to 4 ( 0 - program name)
    if (argc != 4)
    {
        usage();
        return 0;
    }
    int port = atoi(argv[1]);
    int pool_size = atoi(argv[2]);
    int max_request = atoi(argv[3]);
    int port_len = log_10(port) + 1;
    int pool_size_len = log_10(pool_size) + 1;
    int max_request_len = log_10(max_request) + 1;
    // validate arguments not contains another characters
    if (port_len != strlen(argv[1]) || pool_size_len != strlen(argv[2]) || max_request_len != strlen(argv[3]) || !validatePort(argv[1]))
    {
        usage();
        return 0;
    }
    // create brand new threadpool
    threadpool *t = create_threadpool(pool_size);
    if (!t)
    {
        printf("threadpool failed to create\n");
        return 0;
    }
    // integers to store fd for sockets
    int welcome_sockfd, cur_sockfd;
    struct sockaddr_in serv_addr;

    // create new fd for welcome socket
    welcome_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (welcome_sockfd < 0)
    {
        // validate welcome socket created
        perror("error: create socket failure\n");
        destroy_threadpool(t);
        return EXIT_FAILURE;
    }
    // setup server
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    // bind welcome socket
    if (bind(welcome_sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("error: bind failure");
        destroy_threadpool(t);
        close(welcome_sockfd);
        return EXIT_FAILURE;
    }
    // listen to clients queue set to 5
    if (listen(welcome_sockfd, 5) < 0)
    {
        perror("error: listen failure\n");
        destroy_threadpool(t);
        close(welcome_sockfd);
        return EXIT_FAILURE;
    }
    // int sockets[pool_size]; -- TODO
    for (int i = 0; i < max_request; i++)
    {
        // get the new fd from accept to handle the request
        cur_sockfd = accept(welcome_sockfd, NULL, NULL);
        if (cur_sockfd < 0)
        {
            perror("error: acceppt failure");
        }
        else
        {
            char fd[5];
            sprintf(fd, "%d", cur_sockfd);
            // send_response work to dispatch
            dispatch(t, handle_client, fd);
        }
    }
    close(welcome_sockfd);
    destroy_threadpool(t);
}
