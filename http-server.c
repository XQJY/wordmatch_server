/*
** http-server.c
*/

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

// constants
static char const * const HTTP_200_FORMAT = "HTTP/1.1 200 OK\r\n\
Content-Type: text/html\r\n\
Content-Length: %ld\r\n\r\n";
static char const * const HTTP_200_FORMAT_COOKIE = "HTTP/1.1 200 OK\r\n\
Content-Type: text/html\r\n\
Content-Length: %ld\r\n\
Set-Cookie: sessionID = %ld\r\n\r\n";
static char const * const HTTP_400 = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
static int const HTTP_400_LENGTH = 47;
static char const * const HTTP_404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static int const HTTP_404_LENGTH = 45;
static char const * const NAME_HTML = "<p>Welcome, %s!</p>";
static int const NAME_HTML_LENGTH = 17;

// represents the types of method
typedef enum
{
    GET,
    POST,
    UNKNOWN
} METHOD;

    typedef enum{
    GET_INTRO,
    POST_NAME,
    GET_START,
    POST_QUIT,
    POST_GUESS,
    ENDGAME,
    DISCONNECTED,
    INVALID,
    RETRY
}type;

typedef enum{
    STANDBY,
    PENDING_READY,
    READY
}status;

typedef struct request{
    bool dynamic;
    type reqType;
    char* value;
    bool cookie;
} req;

typedef struct cookie{
    long sessionID;
    char* username;
}cookie;

//static variables
static cookie* cookieLib[10];
static char* wordList;
static int currCookie = 0;
static status stage = STANDBY;
static int player1sock = -1;
static int player2sock = -1;
static int submittedsock = -1;
static int unsettledsock = -1;

static void resetPlayer(int sockfd){
    if(player1sock == sockfd){
        player1sock = -1;
    } else if (player2sock == sockfd){
        player2sock = -1;
    }
}

static int uniqueID(int sessionID){
    int i;
    for(i = 0; i < 10; i++){
        if (cookieLib[i] != NULL){
            if(cookieLib[i]->sessionID == sessionID) return false;
        }
    }
    return true;
}

//generate a cookie, return the sessionID
static long generateCookie(char* username){
    int index;
    long sessionID;

    if(currCookie < 10) {
        index = currCookie;
        cookieLib[index] = (cookie*) malloc(sizeof(cookie));
    }
    else {
        index = currCookie % 10;
        free(cookieLib[index]->username);
    }
    currCookie++;
    do{
        sessionID = rand();
    }while(!uniqueID(sessionID));

    cookieLib[index]-> sessionID = sessionID;
    cookieLib[index]-> username = calloc(sizeof(char), strlen(username) + 1);
    memcpy(cookieLib[index]-> username, username, sizeof(char) * (strlen(username) + 1));

    return sessionID;
}

static char* searchCookie(long sessionID){
    int i;
    for(i = 0; i < 10; i++){
        if(cookieLib[i] != NULL) {
            if (cookieLib[i]->sessionID == sessionID)
                return cookieLib[i]->username;
        }
    }
    return NULL;
}

static req* parseRequest(char* buff, int sockfd){
    req* temp = malloc(sizeof(req));
    char * curr = buff;
    METHOD method = UNKNOWN;

    // parse the method
    if (strncmp(curr, "GET ", 4) == 0)
    {
        curr += 4;
        method = GET;
    }
    else if (strncmp(curr, "POST ", 5) == 0)
    {
        curr += 5;
        method = POST;
    }
    else if (write(sockfd, HTTP_400, HTTP_400_LENGTH) < 0)
    {
        perror("write");
        return NULL;
    }

    // sanitise the URI
    while (*curr == '.' || *curr == '/')
        ++curr;

    // parse the type and the value of the request
    if(method == GET) {
        if (strncmp(curr, "?start=Start ", 13) == 0) {
            temp->dynamic = false;
            temp->reqType = GET_START;
            temp->value = NULL;
        }   //read the cookie and return to the start page if the sessionID is stored in the server
            else if ((curr = strstr(buff, "sessionID=")) != NULL) {
            long sessionID = atol(curr + 10);
            if ((curr = searchCookie(sessionID)) != NULL) {
                temp->dynamic = true;
                temp->reqType = POST_NAME;
                temp->value = curr;
                temp->cookie = true;
            } else {
                temp->dynamic = false;
                temp->reqType = GET_INTRO;
                temp->value = NULL;
            }
        }
        else {
            temp->dynamic = false;
            temp->reqType = GET_INTRO;
            temp->value = NULL;
        }
    }

    else if(method == POST){
        if(strstr(buff,"quit=Quit") != NULL){
            temp->dynamic = false;
            temp->reqType = POST_QUIT;
            temp->value = NULL;
        } else if((curr = strstr(buff,"keyword=")) != NULL){
            temp->dynamic = true;
            temp->reqType = POST_GUESS;
            temp->value = curr + 8;
            *strstr(curr,"&") = '\0';
        } else if((curr = strstr(buff,"user=")) != NULL){
            temp->dynamic = true;
            temp->reqType = POST_NAME;
            temp->value = curr + 5;
        } else {
            temp->dynamic = false;
            temp->reqType = INVALID;
            temp->value = NULL;
        }
    }

    else {
        temp->dynamic = false;
        temp->reqType = INVALID;
        temp->value = NULL;
    }

    return temp;
}

static bool response_static_request(type t, int sockfd){
    char* html;

    //decide which html file to read
    if(t == GET_INTRO){
        html = "1_intro.html";
    } else if(t == GET_START){
        html = "3_first_turn.html";
        //change the status to get ready for the game
        if(stage == PENDING_READY) {
            stage = READY;
            //reset the wordList when both player is ready
            free(wordList);
            wordList = NULL;
        }  else {
            stage = PENDING_READY;
        }
    } else if (t == POST_QUIT){
        //reset the status
        stage = STANDBY;
        submittedsock = -1;
        html = "7_gameover.html";
    } else if (t == ENDGAME){
        html = "6_endgame.html";
    }else if (t == RETRY){
        html = "8_retry.html";
    } else if (t == DISCONNECTED){
        html = "9_disconnected.html";
    } else {
        perror("typeError");
        return false;
    }
    // send the HTTP response header
    char buff[2049];
    int n;
    struct stat st;
    stat(html, &st);
    n = sprintf(buff, HTTP_200_FORMAT, st.st_size);
    if (write(sockfd, buff, n) < 0)
    {
        perror("write");
        return false;
    }
    // send the file
    int filefd = open(html, O_RDONLY);
    do{
        n = sendfile(sockfd, filefd, NULL, 2048);
    }
    while (n > 0);
    if (n < 0)
    {
        perror("sendfile");
        close(filefd);
        return false;
    }
    close(filefd);
    return true;
}

static bool response_dynamic_request(req* r, int sockfd){
    int n;
    int move_from;
    char* html;
    char* insertion;
    bool singleWord;
    long added_length;
    char buff[2049];

    //decide which html file to read
    if(r->reqType == POST_NAME){
        html = "2_start.html";
    } else if(r->reqType == POST_GUESS){
        //if the game is completed but not settled for a player, end the game
        if(sockfd == unsettledsock) {
            unsettledsock = -1;
            if (!response_static_request(ENDGAME, sockfd)) {
                return false;
            }
            return true;
        }
        if(stage == READY){
            html = "4_accepted.html";
            //check if both player have submitted the keywords, change stage into completed if so
            if(submittedsock < 0) {
                submittedsock = sockfd;
            } else if (submittedsock != sockfd){
                unsettledsock = submittedsock;
                submittedsock = -1;
                wordList = NULL;
                if(!response_static_request(ENDGAME,sockfd)){
                    return false;
                }
                return true;
            }
        } else if(stage == PENDING_READY){
            //if another player are not ready, return discarded html
            html = "5_discarded.html";
        } else if(stage == STANDBY){
            if (!response_static_request(DISCONNECTED, sockfd)) {
                return false;
            }
            return true;
        } else {
            perror("stage error");
            return false;
        }
    }else {
        perror("typeError");
        return false;
    }

    // get the size of the file
    struct stat st;
    stat(html, &st);
    // increase file size to accommodate the username

    long size;
    //Calculate the addedLength
    if(r->reqType == POST_NAME){
        if(r->cookie) {
            added_length = strlen(r->value) + NAME_HTML_LENGTH;
            size = st.st_size + added_length;
            n = sprintf(buff, HTTP_200_FORMAT, size);
        } else {
            long randID = generateCookie(r->value);
            added_length = strlen(r->value) + NAME_HTML_LENGTH;
            size = st.st_size + added_length;
            n = sprintf(buff, HTTP_200_FORMAT_COOKIE, size, randID);
        }
    } else if(r->reqType == POST_GUESS){
        if(stage == READY){
            if(wordList == NULL) {
                singleWord = true;
                added_length = strlen(r->value) + strlen(" has been ");
                wordList = (char*)calloc(sizeof(char), strlen(r->value) + 1);
                memcpy(wordList, r->value, sizeof(char) * strlen(r->value) + 1);
            }
            else {
                singleWord = false;
                added_length = strlen(wordList) + strlen(", ") + strlen(r->value)  + strlen(" have been ");
                wordList = (char*) realloc(wordList, sizeof(char)* ( added_length + 1 ));
                strcat(wordList, ", ");
                strcat(wordList, r->value);
            }
        } else {
            added_length = strlen(r->value) + strlen(" has been ");
        }
        size = st.st_size + added_length;
        n = sprintf(buff, HTTP_200_FORMAT, size);
    } else {
        perror("typeError");
        return false;
    }

    // send the HTTP response header
    if (write(sockfd, buff, n) < 0)
    {
        perror("write");
        return false;
    }

    // read the content of the HTML file
    int filefd = open(html, O_RDONLY);
    n = read(filefd, buff, 2048);
    if (n < 0)
    {
        perror("read");
        close(filefd);
        return false;
    }
    close(filefd);

    // move the trailing part backward
    if(r->reqType == POST_NAME){
        move_from = ((int) (strstr(buff, "\n<form method=\"GET\">") - buff));
        insertion = (char*) calloc(sizeof(char), added_length);
        sprintf(insertion, NAME_HTML, r->value);
    } else if(r->reqType == POST_GUESS){
        if(stage == READY){
            move_from = ((int) (strstr(buff, "Accepted!") - buff));
            insertion = (char*) calloc(sizeof(char), added_length + 1);
            memcpy(insertion, wordList, sizeof(char)* (added_length +1));
            if(singleWord) {
                strcat(insertion, " has been ");
            } else {
                strcat(insertion, " have been ");
            }
        } else {
            move_from = ((int) (strstr(buff, "Discarded.") - buff));
            insertion = (char*) calloc(sizeof(char), added_length + 1);
            memcpy(insertion, r->value, sizeof(char)* (strlen(r->value) +1));
            strcat(insertion, " has been ");
        }
    } else {
        perror("typeError");
        return false;
    }

    int p1, p2;
    for (p1 = size - 1, p2 = p1 - added_length; p2 >= move_from; --p1, --p2)
        buff[p1] = buff[p2];
    ++p2;

    // put the separator

    strncpy(buff + p2, insertion, added_length);
    free(insertion);

    if (write(sockfd, buff, size) < 0)
    {
        perror("write");
        return false;
    }
    return true;
}

static bool handle_http_request(int sockfd)
{
    // try to read the request
    char buff[2049];
    int n = read(sockfd, buff, 2049);
    if (n <= 0)
    {
        if (n < 0)
            perror("read");
        else
            printf("socket %d close the connection\n", sockfd);
        return false;
    }

    // terminate the string
    buff[n] = 0;

    req * request;

    // return the failure
    if((request = parseRequest(buff,sockfd)) == NULL){
        return false;
    }

    if(sockfd != player1sock && sockfd != player2sock) {
        request -> reqType = RETRY;
        request-> dynamic = false;
    }

    if (request->reqType == INVALID){
        fprintf(stderr, "no other methods supported");
        if (write(sockfd, HTTP_404, HTTP_404_LENGTH) < 0)
        {
            perror("write");
            free(request);
            return false;
        }
    } else if (request->dynamic == false){
        if(!response_static_request(request->reqType, sockfd)){
            free(request);
            return false;
        }
    } else {
        if(!response_dynamic_request(request, sockfd)){
            free(request);
            return false;
        }
    }

    free(request);
    return true;
}

int main(int argc, char * argv[])
{
    srand (time(NULL));

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s ip port\n", argv[0]);
        return 0;
    }

    // create TCP socket which only accept IPv4
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // reuse the socket if possible
    int const reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) < 0)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // create and initialise address we will listen on
    struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    // if ip parameter is not specified
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    // bind address to socket
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // listen on the socket
    listen(sockfd, 5);

    // initialise an active file descriptors set
    fd_set masterfds;
    FD_ZERO(&masterfds);
    FD_SET(sockfd, &masterfds);
    // record the maximum socket number
    int maxfd = sockfd;

    while (1)
    {
        // monitor file descriptors
        fd_set readfds = masterfds;
        if (select(FD_SETSIZE, &readfds, NULL, NULL, NULL) < 0)
        {
            perror("select");
            exit(EXIT_FAILURE);
        }

        // loop all possible descriptor
        for (int i = 0; i <= maxfd; ++i)
            // determine if the current file descriptor is active
            if (FD_ISSET(i, &readfds))
            {
                // create new socket if there is new incoming connection request
                if (i == sockfd)
                {
                    struct sockaddr_in cliaddr;
                    socklen_t clilen = sizeof(cliaddr);
                    int newsockfd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
                    if(player1sock < 0){
                        player1sock = newsockfd;
                    } else if (player2sock < 0){
                        player2sock = newsockfd;
                    }
                    if (newsockfd < 0)
                        perror("accept");
                    else
                    {
                        // add the socket to the set
                        FD_SET(newsockfd, &masterfds);
                        // update the maximum tracker
                        if (newsockfd > maxfd)
                            maxfd = newsockfd;
                        // print out the IP and the socket number
                        char ip[INET_ADDRSTRLEN];
                        printf(
                                "new connection from %s on socket %d\n",
                                // convert to human readable string
                                inet_ntop(cliaddr.sin_family, &cliaddr.sin_addr, ip, INET_ADDRSTRLEN),
                                newsockfd
                        );
                    }
                }
                    // a request is sent from the client
                else if (!handle_http_request(i))
                {
                    resetPlayer(i);

                    if(unsettledsock == i){
                        unsettledsock = -1;
                    }

                    if(submittedsock == i ){
                        submittedsock = -1;
                    }

                    close(i);
                    FD_CLR(i, &masterfds);
                }
            }
    }

    return 0;
}