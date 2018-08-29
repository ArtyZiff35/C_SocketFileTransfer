#include    <stdlib.h>
#include    <string.h>
#include    <inttypes.h>
#include    <sys/stat.h>
#include    <sys/wait.h>
#include    <fcntl.h>
#include    <signal.h>
#include    <unistd.h>
#include    "../errlib.h"
#include    "../sockwrap.h"

#define TIMEOUT 10     //Timeout for waiting client messages before closing connection
#define SEND_TIMEOUT 15   //Timeout for waiting to send data to client
#define BUF_SIZE 2048
#define SMALL_BUF_SIZE 1024
#define SEND_BUF_SIZE 8192
#define MSG_FILE 1
#define MSG_QUIT 0
#define MSG_UNKNOWN -1
#define BACKLOG 3       //Max number of clients waiting to be served

/* GLOBAL VARIABLES */
char *prog_name;

/* FUNCTIONS */
void childHandler(int sig);
void serverService(int mySocket);
int getResponseType(char *message);
int sendMessage(int mySocket, char *message, int size);
int checkFileExistence(char *fileName, char *filePath);
int sendFile(int mySocket, char *fileName);
int recvWithTimeout(int mySocket, void * buffer, size_t buflen, int flags);
int sendWithTimeout(int mySocket, const void * buffer, size_t buflen, int flags);

/*CONCURRENT SERVER: this server is able to serve multiple clients at the same time. When a child process dies, it is collected
  through a SIGCHLD signal handler. Thanks to this there won't be any zombie process. The server is only allowed to serve request 
  for files that are located in the server's working directory or below. Absolute paths and files in upper directories are refused.
  This server can be contacted through Ipv4 and Ipv6 addresses. */

int main(int argc, char **argv)
{
  uint16_t serverPort_n, serverPort_h;        //Server's port
  int mySocket, requestSocket, clientSocket;  //Sockets
  struct sockaddr_in6 serverSockaddr;         //Server's sockaddr
  struct sockaddr_in6 clientSockaddr;         //Client's sockaddr
  socklen_t clientSockaddrLen;
  int serverBacklog = BACKLOG;                //Max length of queue for pending connections
  int pid;
  char ipstr[16];                             //Ip address of server
  int no = 0;                                 //Option used to set the socket to accept both Ipv4 and Ipv6 connections
  char incomingAddr[45]; 	              //Holds textual rapresentation of connecting address


  if(argc != 2)
    {
      fprintf(stderr, "Wrong number of arguments. Correct Usage: <serverPort>\n");
      return -1;
    }


  //Program name is saved as a global variable
  prog_name = argv[0];

  //Server Port number is retrieved
  if(sscanf(argv[1], "%" SCNu16, &serverPort_h) != 1)
    {
      fprintf(stderr,"Invalid port!\n");
      return -1;
    }
  serverPort_n = htons(serverPort_h);                  //Converts notation of the port

  //Creating the server's Socket
  mySocket = Socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  setsockopt(mySocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&no, sizeof(no));   //Option to accept also Ipv4 connections
  printf("Socket number %d created. Timeout: %ds. Buffer size: %d bytes\n", mySocket, TIMEOUT, SEND_BUF_SIZE);


  //Binding the Socket to the Server
  bzero(&serverSockaddr, sizeof(serverSockaddr));
  serverSockaddr.sin6_family = AF_INET6;
  serverSockaddr.sin6_port = serverPort_n;
  serverSockaddr.sin6_addr = in6addr_any;
  serverSockaddr.sin6_scope_id = 0x2;       //Binding to Link-Local scope
  Bind(mySocket, (struct sockaddr *) &serverSockaddr, sizeof(serverSockaddr));
  inet_ntop(AF_INET6, &serverSockaddr.sin6_addr, ipstr, sizeof(ipstr));
  printf("Bound server to address %s (0.0.0.0) at port %d\n",ipstr,serverPort_h);


  //Server starts Listening
  Listen(mySocket, serverBacklog);          //When the queue of pending connections reaches backlog value, ECONNREFUSED error is received by the client
  printf("Socket %d listening with backlog %d\n", mySocket, serverBacklog);

  //Finally we prepare requestSocket that need to be bound
  requestSocket = mySocket;

  //We prepare the main so that it will handle SICHLD
  signal(SIGCHLD, childHandler);

  //SERVER LOOP
  while(1)
    {
      printf("\n");
      //Accepting new connection
      clientSockaddrLen = sizeof(clientSockaddr);
      clientSocket = accept(requestSocket, (struct sockaddr *) &clientSockaddr, &clientSockaddrLen);
      
      if(clientSocket < 0)
	{
	  fprintf(stderr, "Could not accept a connection!\n");
	}
      else
	{
	  if( (pid = fork()) < 0 ) 
	    {
	      //Could not fork correctly
	      fprintf(stderr,"Could not accept a connection!\n");
	      close(clientSocket);
	    }
	  else if (pid > 0)
	    { 
	      //PARENT -> Go accept another connection
	      close(clientSocket);  
	    }
	  else
	    {
	      //CHILD -> Service the just accepted connection
	      inet_ntop(AF_INET6, &clientSockaddr.sin6_addr, incomingAddr, sizeof(incomingAddr));
	      printf("\n---Accepting conn on socket %d from %s at port %d with child %d\n", clientSocket, incomingAddr, ntohs(clientSockaddr.sin6_port),getpid());
	      close(requestSocket);
	      serverService(clientSocket);
	      printf("\n---Terminating child %d\n",getpid());
	      return 0;
	    }
	}   
    }
}





//This function executes the server's duty for an accepted client
void serverService(int mySocket)
{
  fd_set  mySet;                //Set of sockets for Select
  struct timeval myTimeval;     //Timeval for Select timeout
  char *receivingBuffer, *filePath;
  int result;


  //LOOP: WAITING CLIENT MESSAGEs
  while(1)
    {
      //Preparing data structures needed for Select
      FD_ZERO(&mySet);             
      FD_SET(mySocket, &mySet);           //Our socket is put inside the set
      myTimeval.tv_sec = TIMEOUT;         //We set the timeout for the response
      myTimeval.tv_usec = 0;

      //A Select is used in order to know if the client is sending something before timeout
      result = select(FD_SETSIZE, &mySet, NULL, NULL, &myTimeval);
      if(result > 0)
	{
	  //--------------------------------------------------------
	  //If we're here, client is sending data or has disconnected
	  int rec, type;
	  
	  receivingBuffer = (char *) calloc(sizeof(char), BUF_SIZE);
	  if(receivingBuffer == NULL)
	    {
	      fprintf(stderr, "[%d]Failed memory allocation!\n",getpid());
	      close(mySocket);
	      return;
	    }

	  rec = recv(mySocket, receivingBuffer, BUF_SIZE, 0);
	  if(rec > 0)
	    {
	      //This is the case of data available to read (Client has successfully sent a message)
	      type = getResponseType(receivingBuffer);
	      switch(type)
		{
		case MSG_FILE:
		  printf("[%d]GET message received.\n",getpid());
		  filePath = (char*) calloc(sizeof(char), SMALL_BUF_SIZE);
		  if(filePath == NULL)
		    {
		      fprintf(stderr,"[%d]Error during memory allocation\n",getpid());
		      free(receivingBuffer);
		      close(mySocket);
		      return;
		    }
  		  if( checkFileExistence(receivingBuffer, filePath) == 0 )
		    {
		      //If file exists we send it to the client
		      if(sendFile(mySocket, filePath) != 0)
			{
			  fprintf(stderr, "[%d]Could not send file. Closing connection with client\n",getpid());
			  free(receivingBuffer);
			  free(filePath);
			  close(mySocket);
		          return;
			}
		      else
			{
			  printf("[%d]File successfully sent!\n",getpid());
			  free(receivingBuffer);
			  free(filePath);
			}
		    }
		  else
		    {
		      //If file does not exists
		      if(sendMessage(mySocket, "-ERR\r\n", 6) == 0)
			printf("[%d]-ERR message sent to the client due to FILE NOT FOUND or PERMISSION DENIED.\nClosing connection.\n",getpid());
		      free(receivingBuffer);
                      free(filePath);
		      close(mySocket);
		      return;
		    }
		  break;


		  
		case MSG_QUIT:
		  printf("[%d]QUIT message received. Proceeding to close the connection...\n",getpid());
		  free(receivingBuffer);
		  close(mySocket);
		  return;


		  
		case MSG_UNKNOWN:
		  if(sendMessage(mySocket, "-ERR\r\n", 6) == 0)
		    printf("[%d]-ERR message sent to the client due to UNKNOWN COMMAND. Closing connection.\n",getpid());
		  free(receivingBuffer);
		  close(mySocket);
		  return;


		  
		default:
		  fprintf(stderr, "[%d]An error has occurred.\n",getpid());
		  free(receivingBuffer);
		  close(mySocket);
		  return;
		}
	    }
	  else if (rec == 0)
	    {
	      //This is the case in which the client has disconnected
	      fprintf(stderr, "[%d]The client has disconnected!\n",getpid());
	      free(receivingBuffer);
	      close(mySocket);
	      return;
	    }
	  else
	    {
	      //Case of errors
	      fprintf(stderr, "[%d]Error while receiving message\n",getpid());
	      free(receivingBuffer);
	      close(mySocket);
	      return;
	    }
	  //------------------------------------------------------
	}
      else if(result == 0)
	{
	  //If we're here, TIMEOUT has expired
	  fprintf(stderr,"[%d]TIMEOUT: client on socket %d did not reply within %d seconds\n",getpid(),mySocket,TIMEOUT);
	  close(mySocket);
	  return;
	}
      else
	{
	  //Case of failure
	  fprintf(stderr, "[%d]Error during Select\n",getpid());
	  close(mySocket);
	  return;
	}

      //We eventually wait for the next message coming from the client
      printf("\n");
    }
  
}



//This function returns the type of message that was just received.
int getResponseType(char *message)
{
  if(strcmp(message,"QUIT\r\n") == 0)
    {
      return MSG_QUIT;
    }
  else if( strstr(message, "GET ") == message )
    {
      return MSG_FILE;
    }
  else
    {
      return MSG_UNKNOWN;
    }
}


//This function sends a message to the client
int sendMessage(int mySocket, char *message, int size)
{
  int result;

  result = sendn(mySocket, message, size, MSG_NOSIGNAL); //Thanks to MSG_NOSIGNAL, no SIGPIPE 
  if(result == size)
    {
      return 0;
    }
  else
    {
      printf("[%d]Could not send a message\n",getpid());
      return -1;
    }
}


//This function checks if a file exists and if it is allowed to be sent. In case of success it returns 0. Otherwise -1.
int checkFileExistence(char *fileName, char *filePath)
{
  char name[SMALL_BUF_SIZE];
  char *tmp = NULL;
  
  bzero(name, SMALL_BUF_SIZE);
  //We use a buffer for the file name so that we can extract only the name of the file with no other chars
  fileName += 4;                                   //We offset the pointer to point the filename
  strncpy(name, fileName, strlen(fileName)-2);     //Last 2 chars are not copied
  fileName -= 4;                                   //Pointer is reset to initial state so that it can be freed
  
  //Stopping absolute paths
  if(name[0]=='/')
    {
      printf("Cannot accept absolute paths. Only file names and relative paths reachable from working directory.\n");
      return -1;
    }
  
  //We check if the user is going above working directory. This is not allowed.
  char* tmpName = strdup(name);
  char* token = strtok(tmpName, "/");
  int up=0,down=0;
  while (token) 
    {
      if(strcmp(token,"..")==0)
	{
	  up++;
	  if(up > down)
	    {
	      fprintf(stderr,"[%d]Warning! Trying to access above server's working directory!\n",getpid());
	      free(tmpName);
	      return -1;
	    }
	}
      else if(strcmp(token,".")!=0)
	{
	  down++;
	}
      
      token = strtok(NULL, "/");
    }
  down--;
  free(tmpName);
  if(up > down)
    {
      fprintf(stderr,"[%d]Warning! Trying to access above server's working directory!\n",getpid());
      return -1;
    }
  
  //We resolve the absolute path of the requested file
  tmp = realpath(name, tmp);                          //This is the absolute path for the file
  if(tmp == NULL)
    {
      //Case of non existent file
      return -1;
    }
  strcpy(filePath, tmp);
  free(tmp);

  
  //File is accessible in server's working directory or below
  printf("[%d]About to send file %s\n",getpid(),filePath);
  return 0;
}


//This function sends the file (together with its message) whose existence has already been checked
//Correct structure is "+OK\r\n_SIZE__TIME__CONTENT"
int sendFile(int mySocket, char *fileName)
{
  char  *message;
  int fd;
  struct stat fileStat;
  uint32_t fileSize_h, fileTime_h;
  uint32_t fileSize_n, fileTime_n;


  //-----------------------------------------------------------
  //File is opened and fstat is used to retrieve details
  if((fd = open(fileName, O_RDONLY)) < -1)
    return -1;
 
  if(fstat(fd,&fileStat) < 0)
    {
      close(fd);
      return -1;
    }

  //We check if the file is a folder
  if(!S_ISREG(fileStat.st_mode))
    {
      if(sendMessage(mySocket, "-ERR\r\n", 6) == 0)
	printf("[%d]-ERR message sent to the client due to FILE REQUESTED IS NOT A REGULAR FILE. Closing connection.\n",getpid());
      close(fd);
      return -1;
    }

  
  //File size and timestamp are retrieved
  fileSize_h = (uint32_t) fileStat.st_size;
  fileTime_h = (uint32_t) fileStat.st_mtime;

  printf("[%d]File size is %u and timestamp is %u\n",getpid(),fileSize_h,fileTime_h);

  //Now we convert size and timestamp to net byte order and send the first part of the message
  fileSize_n = htonl(fileSize_h);
  fileTime_n = htonl(fileTime_h);
  if( sendMessage(mySocket, "+OK\r\n", 5) != 0)
    {
      close(fd);
      return -1;
    }
  message = (char*) calloc(sizeof(char), 8);
  if(message == NULL)
    {
      close(fd);
      return -1;
    }
  memcpy(message, &fileSize_n, 4);
  message += 4;
  memcpy(message, &fileTime_n, 4);
  message -= 4;
  if( sendMessage(mySocket, message, 8) != 0 )
    {
      close(fd);
      free(message);
      return -1;
    }
  free(message);
  
  //-----------------------------------------------------------
  //Finally we send the actual file
  size_t nread = 0;
  size_t nwritten = 0;
  int result = 0;

  message = (char*) calloc(sizeof(char), SEND_BUF_SIZE);
  if(message == NULL)
    {
      close(fd);
      return -1;
    }
  
  printf("[%d]Sending...\n",getpid());
  while( (nread = read(fd, message, SEND_BUF_SIZE)) > 0 && nwritten < fileSize_h)       //We read and send chunks of SEND_BUF_SIZE size
    {
      //Each time we send only the chunk that we've read
      result = sendWithTimeout(mySocket, message, nread, MSG_NOSIGNAL);  //Thanks to MSG_NOSIGNAL, no SIGPIPE
      if(result != nread)
	{
	  free(message);
	  close(fd);
	  return -1;
	}
      nwritten += result;
      bzero(message, SEND_BUF_SIZE);

    }

  free(message);
  close(fd);

  if(nwritten != fileSize_h)
    return -1;
  
  return 0;
}


//This function implements recv using Select for Timeout purposes
int recvWithTimeout(int mySocket, void * buffer, size_t buflen, int flags)
{

  fd_set  mySet;                //Set of sockets for Select
  struct timeval myTimeval;
  ssize_t nread;
  size_t nleft = buflen;
  int result;

  while(nleft > 0)
    {
      FD_ZERO(&mySet);             
      FD_SET(mySocket, &mySet);           //Our socket is put inside the set
      myTimeval.tv_sec = TIMEOUT;         //We set the timeout for the response
      myTimeval.tv_usec = 0;
      
      //A Select is used in order to know if the client is sending something before timeout
      result = select(FD_SETSIZE, &mySet, NULL, NULL, &myTimeval);
      if( result > 0)
	{
	  //If we're here, client is sending data or has disconnected
	  nread = recv(mySocket, buffer, nleft, flags);
	  if(nread > 0)
	    {
	      //We have received some data
	      nleft -= nread;
	      buffer += nread;
	    }
	  else if(nread == 0)
	    {
	      //Connection was closed
	      fprintf(stderr,"[%d]Connection closed by other party!\n",getpid());
	      return (buflen - nleft);
	    }
	  else
	    {
	      //An error was received
	      fprintf(stderr,"[%d]Error during recv\n",getpid());
	      return -1;
	    }
	}
      else if(result == 0)
	{
	  //If we're here, TIMEOUT has expired
	  fprintf(stderr,"[%d]TIMEOUT has expired during recv!\n",getpid());
	  return -2;
	}
      else
	{
	  //Case of failure of select
	  fprintf(stderr,"[%d]Error during Select\n",getpid());
	  return -3;
	}
    }
  
  return (buflen - nleft);
}

//This function sends with a timeout, in case of full sending buffer
int sendWithTimeout(int mySocket, const void * buffer, size_t buflen, int flags)
{
  size_t nleft = buflen;
  ssize_t nwritten;
  fd_set  mySet;                //Set of sockets for Select
  struct timeval myTimeval;
  int result;
  
  while(nleft > 0)
    {
      FD_ZERO(&mySet);             
      FD_SET(mySocket, &mySet);           //Our socket is put inside the set
      myTimeval.tv_sec = TIMEOUT;         //We set the timeout 
      myTimeval.tv_usec = 0;
      
      //A Select is used in order to know if we can send more data
      result = select(FD_SETSIZE, NULL, &mySet, NULL, &myTimeval);
      if( result > 0)
	{
	  //If we're here, we are able to send new data
	  nwritten = send(mySocket, buffer, buflen, flags);
	  if(nwritten > 0)
	    {
	      //We have sent some data
	      nleft -= nwritten;
	      buffer += nwritten;
	    }
	  else
	    {
	      //An error was received
	      fprintf(stderr,"[%d]Error during send\n",getpid());
	      return -1;
	    }
	}
      else if(result == 0)
	{
	  //If we're here, TIMEOUT has expired
	  fprintf(stderr,"[%d]TIMEOUT has expired during send!\n",getpid());
	  return -2;
	}
      else
	{
	  //Case of failure of select
	  fprintf(stderr,"[%d]Error during Select\n",getpid());
	  return -3;
	}
    }
  
  return (buflen - nleft);
}


//This function is called to collect dead children processes on SIGCHLD receival
void childHandler(int sig)
{
  pid_t pid;

  pid = wait(NULL);
  printf("(Pid %d was collected).\n", pid);
}
