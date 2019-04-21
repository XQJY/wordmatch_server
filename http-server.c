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

// represents the types of requests
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

// represents the current status of the server
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
static char* wordList1[20];
static char* wordList2[20];
static int currCookie = 0;
static status stage = STANDBY;
static int player1sock = -1;
static int player2sock = -1;
static int unsettledsock = -1;
static int currentRound = -1;

//Enter the next round, assign a new and different picture ID for the server
static void nextRound(){
    if(currentRound == -1){
        currentRound = rand()%4 + 1;
    } else currentRound = (currentRound + rand() % 3) % 4 + 1;
}

//If a player start a game, save the sockfd for that player
static void setPlayer(int sockfd){
    if(player1sock < 0){
        player1sock = sockfd;
    } else if (player2sock < 0){
        player2sock = sockfd;
    }
}

//To tell whether the current socket is playing the game
static bool isPlayer(int sockfd){
    return player1sock == sockfd || player2sock ==sockfd;
}

/*If one of the players triggers the completion of the game
  the other player is marked as unsettled*/
static void record_unsettled(int sockfd){
    if(player1sock == sockfd){
        unsettledsock = player2sock;
    } else if (player2sock == sockfd){
        unsettledsock = player1sock;
    }
}

//Clear the caches for the old game
static void reset(){
    int i = 0;
    player1sock = -1;
    player2sock = -1;
    while (i < 20 && wordList1[i] != NULL) {
       free(wordList1[i]);
       wordList1[i] = NULL;
       i++;
    }

    i = 0;
    while (i < 20 && wordList2[i] != NULL) {
        free(wordList2[i]);
        wordList2[i] = NULL;
        i++;
    }
}

//return the wordlist of the current player
static char** listOf(int sockfd){
    if(player1sock == sockfd){
        return wordList1;
    } else if (player2sock == sockfd){
        return wordList2;
    } else return NULL;
}

//return the wordlist of the current opponent
static char** listOfOpponent(int sockfd){
    if(player1sock == sockfd){
        return wordList2;
    } else if (player2sock == sockfd){
        return wordList1;
    } else return NULL;
}

//Concatenate the wordList to be a single string
static char* concatenateList(int sockfd){
    int len = 0;
    int i = 0;
    int j = 0;
    char* temp;
    char** tempList = listOf(sockfd);

    while(i < 20 && tempList[i] != NULL){
        len += strlen(tempList[i]);
        i++;
    }

    len += 2 * (i-1);
    temp = calloc(sizeof(char), len + 1);

    while(j < 20 && tempList[j] != NULL){
        strcat(temp, tempList[j]);
        if(j != i -1) strcat(temp, ", ");
        j++;
    }

    return temp;
}

//check if the word matches any word in the opponent's wordList
static bool match(char* word, int sockfd){
    int i = 0;
    char ** tempList = listOfOpponent(sockfd);

    while(i < 20 && tempList[i] != NULL){
      if(!strcmp(tempList[i], word))  return true;
      i++;
    }

    return false;
}

//Check whether it's a duplicate sessionID
static int uniqueID(int sessionID){
    int i;
    for(i = 0; i < 10; i++){
        if (cookieLib[i] != NULL){
            if(cookieLib[i]->sessionID == sessionID) return false;
        }
    }
    return true;
}


//Generate a cookie, return the sessionID
static long generateCookie(char* username){
    int index;
    long sessionID;

    //The server can only remeber the recent 10 visitors
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

//Give a cookie, return the corresponding name of that sessionID
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

//Parse the Request header to a structure type
static req* parseRequest(char* buff, int sockfd){
    req* temp = malloc(sizeof(req));
    char * curr = buff;
    METHOD method = UNKNOWN;

    // Parse the method
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

    // Sanitise the URI
    while (*curr == '.' || *curr == '/')
        ++curr;

    // Parse the type and the value of the request
    if(method == GET) {
        if (strncmp(curr, "?start=Start ", 13) == 0) {
            temp->dynamic = true;
            temp->reqType = GET_START;
            temp->value = NULL;
            temp->cookie = false;
        }
        //read the cookie and return to the start page if the sessionID is stored in the server
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
                temp->cookie = false;
            }
        }
        else {
            temp->dynamic = false;
            temp->reqType = GET_INTRO;
            temp->value = NULL;
            temp->cookie = false;
        }
    }

    else if(method == POST){
        if(strstr(buff,"quit=Quit") != NULL){
            temp->dynamic = false;
            temp->reqType = POST_QUIT;
            temp->value = NULL;
            temp->cookie = false;
        } else if((curr = strstr(buff,"keyword=")) != NULL){
            temp->dynamic = true;
            temp->reqType = POST_GUESS;
            temp->value = curr + 8;
            temp->cookie = false;
            *strstr(curr,"&") = '\0';
        } else if((curr = strstr(buff,"user=")) != NULL){
            temp->dynamic = true;
            temp->reqType = POST_NAME;
            temp->value = curr + 5;
            temp->cookie = false;
        } else {
            temp->dynamic = false;
            temp->reqType = INVALID;
            temp->value = NULL;
            temp->cookie = false;
        }
    }

    else {
        temp->dynamic = false;
        temp->reqType = INVALID;
        temp->value = NULL;
        temp->cookie = false;
    }

    return temp;
}

//Simply send the HTML file if there's nothing need to be changed
static bool response_static_request(type t, int sockfd){
    char* html;
    //decide which html file to read
    if(t == GET_INTRO){
        html = "1_intro.html";
    }  else if (t == POST_QUIT){
        //reset the status if the request is from a current player
        if(isPlayer(sockfd)) {
            stage = STANDBY;
            reset();
        }
        html = "7_gameover.html";
    } else if (t == ENDGAME){
        html = "6_endgame.html";
    } else if (t == RETRY){
        html = "8_retry.html";
    } else if (t == DISCONNECTED){
        html = "9_disconnected.html";
    } else {
        perror("typeError");
        return false;
    }
    // Send the HTTP response header
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
    // Send the file
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

//Edit the html file in the buffer and send it to the client
static bool response_dynamic_request(req* r, int sockfd){
    int n = 0;
    int i = 0;
    int move_from = 0;
    char ** wordList = NULL;
    char* html = NULL;
    char* insertion = NULL;
    bool singleWord = false;
    long added_length = 0;
    char buff[2049];

    //Decide which html file to read
    /**if name is posted**/
    if(r->reqType == POST_NAME){
        html = "2_start.html";
    }
    /**if guess is attemptted**/
    else if(r->reqType == POST_GUESS){
        //If the game is completed but not settled, end the game for the player
        if(sockfd == unsettledsock) {
            unsettledsock = -1;
            if (!response_static_request(ENDGAME, sockfd)) {
                return false;
            }
            return true;
        }
        //If the player is not playing the current game, show error messages
        else if(!isPlayer(sockfd)){
            if (!response_static_request(DISCONNECTED, sockfd)) {
                return false;
            }
            return true;
        }
        if(stage == READY){
            html = "4_accepted.html";
            //Check if the player have a match
            if(match(r->value, sockfd)) {
                //if so, reset the game and record the opponent's status as unsettled
                record_unsettled(sockfd);
                stage = STANDBY;
                reset();
                //End the game
                if(!response_static_request(ENDGAME,sockfd)){
                    return false;
                }
                return true;
            }
        } else if(stage == PENDING_READY){
            //If another player is not ready, return the discarded html
            html = "5_discarded.html";
        }   //If the other player has left the game, show error messages
        else if(stage == STANDBY){
            if (!response_static_request(DISCONNECTED, sockfd)) {
                return false;
            }
            return true;
        } else {
            perror("stage error");
            return false;
        }
    }
    /**if start button is pressed**/
    else if(r->reqType == GET_START){
        html = "3_first_turn.html";
        //if the player is already in a game, i.e refresh the page, end game
        if(isPlayer(sockfd)){
            r->reqType = POST_QUIT;
            if(!response_static_request(POST_QUIT,sockfd)){
                return false;
            }
            return true;
        }
        //Change the status to get ready for the game
        if(stage == STANDBY){
            setPlayer(sockfd);
            nextRound();
            stage = PENDING_READY;
        }
        //Start the game by advancing the status again
        else if(stage == PENDING_READY) {
            setPlayer(sockfd);
            stage = READY;
        }
        //Prompt retry message if there are no vacancy for a new player
        else if(!response_static_request(RETRY,sockfd)){
            return false;
        }
    } else {
        perror("typeError");
        return false;
    }

    // get the size of the file
    struct stat st;
    stat(html, &st);
    // increase file size to accommodate the username

    long size = 0;
    //Calculate the addedLength, and generate the header
    if(r->reqType == POST_NAME){
        if(r->cookie) {
            /*The response header for POST_NAME request with a valid cookie,
            which is redirected from a GET Request*/
            added_length = strlen(r->value) + NAME_HTML_LENGTH;
            size = st.st_size + added_length;
            n = sprintf(buff, HTTP_200_FORMAT, size);
        } else {
            /*The response header for POST_NAME request without a valid cookie,
            assign and send the COOKIE to the client*/
            long randID = generateCookie(r->value);
            added_length = strlen(r->value) + NAME_HTML_LENGTH;
            size = st.st_size + added_length;
            n = sprintf(buff, HTTP_200_FORMAT_COOKIE, size, randID);
        }
    } else if(r->reqType == POST_GUESS){
        if(stage == READY){
          /*Calculate the header of a single word Accepted Page*/
            wordList = listOf(sockfd);
            if(wordList[0] == NULL) {
                singleWord = true;
                added_length = strlen(r->value) + strlen(" has been ");
                wordList[0] = (char*)calloc(sizeof(char), strlen(r->value) + 1);
                memcpy(wordList[0], r->value, sizeof(char) * strlen(r->value) + 1);
            }
            else {
            /*Calculate the header of mplti-word Accepted Page*/
                singleWord = false;
                while(i < 20){
                    if(wordList[i] == NULL) {
                        wordList[i] = (char*)calloc(sizeof(char), strlen(r->value) + 1);
                        memcpy(wordList[i], r->value, sizeof(char) * strlen(r->value) + 1);
                        break;
                    }
                    i++;
                }
                //Also convert the word list to a single string
                insertion = concatenateList(sockfd);
                added_length = strlen(insertion) + strlen(" have been ");
            }
        }/*Calculate the header of a Discarded Page*/
         else {
            added_length = strlen(r->value) + strlen(" has been ");
        }
        size = st.st_size + added_length;
        n = sprintf(buff, HTTP_200_FORMAT, size);
    }/*Calculate the header of a First-turn Page*/
    else if(r->reqType == GET_START){
        size = st.st_size;
        n = sprintf(buff, HTTP_200_FORMAT, size);
    }

    // Send the HTTP response header
    if (write(sockfd, buff, n) < 0)
    {
        perror("write");
        return false;
    }

    // Read the content of the HTML file
    int filefd = open(html, O_RDONLY);
    n = read(filefd, buff, 2048);
    if (n < 0)
    {
        perror("read");
        close(filefd);
        return false;
    }
    close(filefd);

    // Calculate the variables for editing the html page
    if(r->reqType == POST_NAME){
        move_from = ((int) (strstr(buff, "\n<form method=\"GET\">") - buff));
        insertion = (char*) calloc(sizeof(char), added_length);
        sprintf(insertion, NAME_HTML, r->value);
    } else if(r->reqType == POST_GUESS){
        if(stage == READY){
            move_from = ((int) (strstr(buff, "Accepted!") - buff));
            if(singleWord) {
                insertion = (char*) calloc(sizeof(char), added_length + 1);
                memcpy(insertion, wordList[0], sizeof(char)* (added_length +1));
                strcat(insertion, " has been ");
            } else {
                insertion = realloc(insertion, sizeof(char)* (added_length +1));
                strcat(insertion, " have been ");
            }
        } else {
            move_from = ((int) (strstr(buff, "Discarded.") - buff));
            insertion = (char*) calloc(sizeof(char), added_length + 1);
            memcpy(insertion, r->value, sizeof(char)* (strlen(r->value) +1));
            strcat(insertion, " has been ");
        }
    }
    //Move the trailing part backward, the first_turn page don't have any insertion
    if(r->reqType != GET_START){
        int p1, p2;
        for (p1 = size - 1, p2 = p1 - added_length; p2 >= move_from; --p1, --p2)
            buff[p1] = buff[p2];
        ++p2;

        // Put the separator
        strncpy(buff + p2, insertion, added_length);
        free(insertion);
    }

    //Change the picture
    if(r->reqType != POST_NAME)
    *(strstr(buff, ".jpg") - 1) = (char) (currentRound + 48);

    //Send the page
    if (write(sockfd, buff, size) < 0)
    {
        perror("write");
        return false;
    }
    return true;
}

//Swicther of responses
static bool handle_http_request(int sockfd)
{
    // Try to read the request
    char buff[2049];
    req * request;
    int n = read(sockfd, buff, 2049);
    if (n <= 0)
    {
        if (n < 0)
            perror("read");
        else
            printf("socket %d close the connection\n", sockfd);
        return false;
    }

    // Terminate the string
    buff[n] = 0;

    // Return the failure
    if((request = parseRequest(buff,sockfd)) == NULL){
        return false;
    }

    //Handle INVALID requests
    if (request->reqType == INVALID){
        fprintf(stderr, "no other methods supported");
        if (write(sockfd, HTTP_404, HTTP_404_LENGTH) < 0)
        {
            perror("write");
            free(request);
            return false;
        }
    }
    // Handle static responses
    else if (request->dynamic == false){
        if(!response_static_request(request->reqType, sockfd)){
            free(request);
            return false;
        }
    }
    // Handle dynamic responses
    else {
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
    //initialise the random generator with random seed
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
                    //reset the server parameters when disconnected
                    if(isPlayer(i))reset();

                    if(unsettledsock == i){
                        unsettledsock = -1;
                    }

                    close(i);
                    FD_CLR(i, &masterfds);
                }
            }
    }

    return 0;
}
