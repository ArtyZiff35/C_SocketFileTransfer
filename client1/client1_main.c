#include     <stdlib.h>
#include     <string.h>
#include     <inttypes.h>
#include     <libgen.h>
#include     <errno.h>
#include     <fcntl.h>
#include     <arpa/inet.h>
#include     <sys/time.h> 
#include     "../errlib.h"
#include     "../sockwrap.h"

/* CONSTANTS */
#define MSG_FILE 1
#define MSG_ERROR 0
#define MSG_UNKNOWN -1
#define DOW_BUF_SIZE 8192
#define SMALL_BUF_SIZE 128
#define TIMEOUT 20

/* GLOBAL VARIABLES */
char *prog_name;

/* FUNCTIONS */
int connectManager(char *argv1, char *argv2);
int getIpVersion(char *address);
int getResponseType(int mySocket);
int receiveFile(int mySocket, char* fileName);
int sendMessage(int mySocket, char* message, int length);
void serverClose(int *mySocket);
int recvWithTimeout(int mySocket, void * buffer, size_t buflen, int flags);
int connectWithTimeout(int mySocket, const struct sockaddr *destaddr, socklen_t addrlen);

/*CLIENT: This client connects through ipv4, ipv6 or Name Resolution to a server. It sends GET messages to request one or more 
  files. Upon receiving a OK message from the server it starts downloading those files: a timeout system has been implement to avoid
  endless wait for server messages. Before ending the interaction it sends a QUIT message and eventually close the connection.*/

int main(int argc, char *argv[])
{
  int mySocket;           //Client Socket
  int result;		  //Variable holding exit status of various		
  char *message;          //Message buffer


  if(argc < 4)
    {
      fprintf(stderr,"Wrong number of arguments. Correct Usage : <serverIPaddress> <serverPort> <file1> <file2> ...\n");
      return -1;
    }
  
  //Program name is saved as a global variable
  prog_name = argv[0];

  //Prepare Socket and connect to the server: manages ipv4, ipv6 and Name Resolution
  mySocket = connectManager(argv[1],argv[2]);
  if(mySocket == -1)
    {
      return -1;
    }

  //Now the Client asks for all the file it needs
  for(int i=3; i<argc; i++)
    {
      //----------------------------------------------------------
      //First of all the GET message is prepared and sent to the server
      int messageLength = 6+strlen(argv[i]);
      message = (char *) calloc ( (messageLength + 1), sizeof(char) );
      if(message == NULL)
	{
	  fprintf(stderr,"Failed memory allocation!\n");
	  close(mySocket);
	  return -1;
	}

      sprintf(message, "GET %s\r\n",argv[i]);               //GET message is prepared with the right notation

      if(sendMessage(mySocket, message, messageLength) != 0)
	{
	  fprintf(stderr, "Could not send GET message\n");
	  close(mySocket);
	  free(message);
	  return -1;
	}
      free(message);

      //----------------------------------------------------------
      //Now the client waits for a Server response
      result = getResponseType(mySocket);
      switch(result)
	{
	case MSG_FILE:
	  //Case of "+OK\r\n" msg correctly received from Server. We proceed to receive the file
	  printf("OK MESSAGE received.\n");
	  if(receiveFile(mySocket, argv[i]) == -2)
	  {
	    fprintf(stderr,"File %s was NOT received!\n",argv[i]);
	    close(mySocket);
	    return 0;
	  }
	  break;

	case MSG_ERROR:
	  //Case of "-ERR\r\n" msg correctly received from Server
	  printf("ERR MESSAGE received for request of %s. Closing connection.\n",argv[i]);
	  close(mySocket);
	  return 0;

	case MSG_UNKNOWN:
	  //Case of unknown message from Server
	  printf("An unknown message was received from the Server! Closing connection.\n");
	  close(mySocket);
	  return 0;

	default:
	  //Case of other errors (Failed allocation, unable to receive from Server...)
	  close(mySocket);
	  fprintf(stderr,"Error during initial reception\n");
	  return 0;
	}
      //--------------------------------------------------------
      //Proceeding to the next file request (if any)
      printf("\n");
    }

  //Close Connection
  serverClose(&mySocket);
  
  exit(0);
}





//This function sends to the server the message specified as argument
int sendMessage(int mySocket, char* message, int length)
{
  int result;
  
  result = sendn(mySocket, message, length, MSG_NOSIGNAL);     //Message is sent to server.
  if(result != length)
    {
      fprintf(stderr,"Could not send message to Server!\n");
      return-1;
    }
  printf("Message sent: %s",message);

  return 0;
}



//This function sends the QUIT message to the server and closes the connection
void serverClose(int *mySocket)
{
  //First of all we send QUIT message to the Server
  if(sendMessage(*mySocket, "QUIT\r\n", 6) != 0)
    fprintf(stderr, "Could not send QUIT message\n");

  Close(*mySocket);
  printf("Connection closed\n\n");
}



//This function's duty is to understand which type of msg the server is trying to send
//Returns: 1=Incoming File // 0=Error msg from Server // -1=Unkown message from Server // -2=Other errors
int getResponseType(int mySocket)
{
  char *receivingBuffer;                //Receiving buffer
  char signBuffer;                      //Sign buffer
  int result;

  //The first response char determines the response type
  result = recvWithTimeout(mySocket, &signBuffer, 1, 0);
  if(result != 1)                                    //If we didn't receive from the server...
    {
      fprintf(stderr,"Could not receive message from Server!\n");
      return -2;
    }

  switch (signBuffer)
    {
    case '+':
      //Awaited "+OK\r\n"
      //If the first char is +, then we check if the remaining part of the message is correct before receiving the file
      receivingBuffer = (char *) calloc (sizeof(char),4+1);
      if(receivingBuffer == NULL)
	{
	  fprintf(stderr,"Failed memory allocation!\n");
	  return -2;
	}
      result = recvWithTimeout(mySocket, receivingBuffer, 4, 0);
      if(result != 4)
	{
	  fprintf(stderr,"Could not receive message from Server (or Unknown message received)!\n");
	  free(receivingBuffer);
	  return -2;
	}
      if(strcmp(receivingBuffer,"OK\r\n")==0)
	{
	  free(receivingBuffer);
	  return MSG_FILE;
	}
      else
	{
	  free(receivingBuffer);
	  return MSG_UNKNOWN;
	}
      break;


      
    case '-':
      //Awaited "-ERR\r\n"
      //If the first char is -, then we check if the remaining part of the message is the Server error msg or an unknown msg
      receivingBuffer = (char *) calloc (sizeof(char),5+1);
      if(receivingBuffer == NULL)
	{
	  fprintf(stderr,"Failed memory allocation!\n");
	  return -2;
	}
      result = recvWithTimeout(mySocket, receivingBuffer, 5, 0);
      if(result != 5)
	{
	  fprintf(stderr,"Could not receive message from Server (or Unknown message received)!\n");
	  free(receivingBuffer);
	  return -2;
	}
      if(strcmp(receivingBuffer,"ERR\r\n")==0)
	{
	  free(receivingBuffer);
	  return MSG_ERROR;
	}
      else
	{
	  free(receivingBuffer);
	  return MSG_UNKNOWN;
	}
      break;
      
      

    default:
      //In this case the first char was already wrong (was not + or -)
      return MSG_UNKNOWN;
      break;
    }
  
  return -2;
}



//This function receives size, timestamp and content of the file from the Server
int receiveFile(int mySocket, char* fileName)
{
  uint32_t fileSize_n, fileSize_h;
  uint32_t fileTime_n, fileTime_h;
  int result;
  char *receivingBuffer;

  //--------------------------------------------------------------------------------------------
  //First of all the function receives size and timestamp (32+32=64BITs=8bytes) in network order
  receivingBuffer = (char*) calloc( sizeof(char) , 8+1 );
  if(receivingBuffer == NULL)
    {
      fprintf(stderr,"Failed memory allocation!\n");
      return -2;
    }
  result = recvWithTimeout(mySocket, receivingBuffer, 8, 0);    
  if(result != 8)
    {
      fprintf(stderr,"Could not receive message from Server (or Unknown message received)!\n");
      free(receivingBuffer);
      return -2;
    }
  memcpy(&fileSize_n, receivingBuffer, 4);    //First 4 bytes of buffer are moved into fileSize_n
  receivingBuffer += 4;                       //Buffer Pointer is incremented to point to next 4 bytes
  memcpy(&fileTime_n, receivingBuffer, 4);    //Last 4 bytes of buffer are moved into fileTime_n
  receivingBuffer -= 4;                       //Buffer pointer is reset to its initial point: we can free it only from start of the block
  free(receivingBuffer);
   
  //We convert size and timestamp from network byte order to host byte order
  fileSize_h = ntohl(fileSize_n);
  fileTime_h = ntohl(fileTime_n);

  //---------------------------------------------------------------------------------------------

  //Now the actual file is received
  FILE *fp;
  size_t nreceived = 0, nwritten = 0, nleft = fileSize_h;
  char *baseName;
  struct timeval startTime, endTime;

  baseName = basename(fileName);         //The base name of the file, so that is saved in root folder of client
  printf("Writing file %s\n",baseName);
  fp = fopen( baseName, "w");
  if(fp == NULL)
    {
      fprintf(stderr,"Could not open File!\n");
      return -2;
    }
  receivingBuffer = (char *) calloc( sizeof(char), DOW_BUF_SIZE);
  if(receivingBuffer == NULL)
    {
      fprintf(stderr, "Could not allocate buffer in memory!\n");
      fclose(fp);
      return -2;
    }

  gettimeofday(&startTime, NULL);   //We calculate in how much time the file is sent
  while(nleft > 0)                  //We receive chuncks of DOW_BUF_SIZE size
    {
      //Each time we write only the chunk that we've received
      if(nleft > DOW_BUF_SIZE)
	{
	  nreceived = recvWithTimeout(mySocket, receivingBuffer, DOW_BUF_SIZE, 0);
	  if(nreceived != DOW_BUF_SIZE)
	    {
	      fclose(fp);
	      free(receivingBuffer);
	      remove(baseName);
	      return -2;
	    }
	}
      else
	{
	  nreceived = recvWithTimeout(mySocket, receivingBuffer, nleft, 0);
	  if(nreceived != nleft)
	    {
	      fclose(fp);
	      free(receivingBuffer);
	      remove(baseName);
	      return -2;
	    }
	}
      nwritten = fwrite(receivingBuffer, sizeof(char), nreceived, fp);   //Write to file what has been received
      if(nreceived != nwritten)
	{
	  fprintf(stderr, "Could not receive file!\n");
	  fclose(fp);
	  free(receivingBuffer);
	  remove(baseName);
	  return -2;
	}
      nleft -= nwritten;
      bzero(receivingBuffer, DOW_BUF_SIZE);

    }
  
  gettimeofday(&endTime, NULL);
  fclose(fp);
  free(receivingBuffer);

  if(nleft != 0)
    {
      fprintf(stderr, "Could not fully receive the file!\n");
      remove(baseName);
      return -2;
    }
  
  printf("File %s successfully transferred in %lds\nSize: %lu and Timestamp: %lu\n",baseName,(endTime.tv_sec - startTime.tv_sec),(unsigned long)fileSize_h,(unsigned long) fileTime_h);

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
	      fprintf(stderr,"Connection closed by other party!\n");
	      return (buflen - nleft);
	    }
	  else
	    {
	      //An error was received
	      fprintf(stderr,"Error during recv\n");
	      return -1;
	    }
	}
      else if(result == 0)
	{
	  //If we're here, TIMEOUT has expired
	  fprintf(stderr,"TIMEOUT has expired during recv!\n");
	  return -2;
	}
      else
	{
	  //Case of failure of select
	  fprintf(stderr,"Error during Select\n");
	  return -3;
	}
    }
  
  return (buflen - nleft);
}


//This function tries to connect in a given Timeout
int connectWithTimeout(int mySocket, const struct sockaddr *destaddr, socklen_t addrlen)
{
  fd_set set;
  int result;
  struct timeval  timeout;

  
  timeout.tv_sec = TIMEOUT;
  timeout.tv_usec = 0;
  
  FD_ZERO(&set);
  FD_SET(mySocket, &set);

  //We temporally set the socket as Non-Blocking
  fcntl(mySocket, F_SETFL, O_NONBLOCK);

  result = connect(mySocket, (struct sockaddr *) destaddr, addrlen);
  
  if(result == -1)
    {
      //The socket is nonblocking and the connection cannot be completed immediately.
      if ( errno != EINPROGRESS )
            return -1;
    }
  
  result = select(FD_SETSIZE, NULL, &set, NULL, &timeout);
  if(result > 0)
    {
      //Something has happened before timeout. We need to check if it is successfull connection or error
      int error;
      socklen_t len = sizeof(error);
      //We set again the socket as Blocking for future recv
      fcntl(mySocket, F_SETFL, fcntl(mySocket, F_GETFL, 0) & ~O_NONBLOCK);
      getsockopt(mySocket, SOL_SOCKET, SO_ERROR, &error, &len);
      if(error == 0)
	return 0;    //Connect was successfull!
      else
	return -1;   //Connect could not enstablish connection
    }
  else if(result == 0)
    {
      //If we're here, TIMEOUT has expired
      fprintf(stderr,"TIMEOUT has expired during Connection!\n");
      return -2;
    }
  else
    {
      //Case of failure of select
      fprintf(stderr,"Error during Select\n");
      return -3;
    }

  return -1;
}



//This function determines if the input address is ipv4 or ipv6
int getIpVersion(char *address)
{
    char tmp[16];
    
    if (inet_pton(AF_INET, address, tmp))
        return 4;
    else if (inet_pton(AF_INET6, address, tmp))
        return 6;

    return -1;
}


//This function tries to connect to ipv4 or ipv6 after having understood the correct type of address. If necessary it resolves Names
int connectManager(char *argv1, char *argv2)
{
  uint16_t serverPort_n, serverPort_h;	//Server port number (net/host order) 
  int mySocket;                         //Client Socket
  int result,ipVer;
  struct sockaddr_in serverSockaddr;	//Server address structure
  struct sockaddr_in6 serverSockaddr6;  //Server address structure for ipv6
  struct in_addr serverIPaddr;         	//Server IP addr structure
  struct in6_addr serverIPaddr6;        //Server IP addr structure for ipv6
  struct hostent *he;                   //Struct for name resolution
  struct in_addr **addr_list;

  //Get target Port from argument 2
  if (sscanf(argv2, "%" SCNu16, &serverPort_h)!=1)     
    err_quit("Invalid port number");
  serverPort_n = htons(serverPort_h);                    //Converts notation of the port for the struct
  
  //Get target IP from argument 1 and understand what type of address it is (ipv4 or ipv6)
  ipVer = getIpVersion(argv1);
  if(ipVer == 4)
    {
      //Case of ipv4
      result = inet_aton(argv1, &serverIPaddr);            //Converts read address from dotted notation to a "struct in_addr"
      if(result == 0)
	{
	  printf("Invalid address string\n");
	  return -1;
	}
      mySocket = Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      printf("Socket number %d created. Timeout: %ds. Buffer size: %d bytes\n",mySocket, TIMEOUT, DOW_BUF_SIZE);	
      bzero(&serverSockaddr, sizeof(serverSockaddr));   //Set every parameter to zero
      serverSockaddr.sin_family = AF_INET;
      serverSockaddr.sin_port   = serverPort_n;
      serverSockaddr.sin_addr   = serverIPaddr;
      showAddr("Trying to connect before Timeout to", &serverSockaddr); 
      result = connectWithTimeout(mySocket, (struct sockaddr *) &serverSockaddr, sizeof(serverSockaddr));
      if(result != 0)
	{
	  fprintf(stderr,"Could not connect.\n");
	  return -1;
	}
      else
	{
	  printf("Connected.\n\n");
	  return mySocket;
	}
    }
  else if (ipVer == 6)
    {
      //Case of ipv6
      printf("Detected ipv6 address\n");
      
      result = inet_pton(AF_INET6, argv1, &serverIPaddr6);
      if(result != 1)
	{
	  printf("Invalid address string\n");
	  return -1;
	}
      printf("Connecting to %s at %s\n",argv1,argv2);
      mySocket = Socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
      printf("Socket number %d created. Timeout: %ds. Buffer size: %d bytes\n",mySocket, TIMEOUT, DOW_BUF_SIZE);
      
      bzero(&serverSockaddr6, sizeof(serverSockaddr6));
      serverSockaddr6.sin6_family = AF_INET6;
      serverSockaddr6.sin6_port = serverPort_n;
      serverSockaddr6.sin6_addr = serverIPaddr6;
      serverSockaddr6.sin6_scope_id = 0x2;       //Link local scope
      result = connectWithTimeout(mySocket, (struct sockaddr *) &serverSockaddr6, sizeof(serverSockaddr6));
      if(result != 0)
	{
	  fprintf(stderr,"Could not connect.\n");
	  return -1;
	}
      else
	{
	  printf("Connected.\n\n");
	  return mySocket;
	}
    }
  else
    {
      //Case of name resolution OR invalid address
      if ( (he = gethostbyname( argv1 ) ) == NULL) 
	{
	  fprintf(stderr,"Invalid address or no connection\n");
	  return -1;
	}
      addr_list = (struct in_addr **) he->h_addr_list;
      mySocket = Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      printf("Socket number %d created. Timeout: %ds. Buffer size: %d bytes\n",mySocket, TIMEOUT, DOW_BUF_SIZE);
      for(int i = 0; addr_list[i] != NULL; i++) 
	{
	  //Try to connect to this resolved address...
	  bzero(&serverSockaddr, sizeof(serverSockaddr));   
	  serverSockaddr.sin_family = AF_INET;
	  serverSockaddr.sin_port   = serverPort_n;
	  serverSockaddr.sin_addr   = *addr_list[i];
	  showAddr("Trying to connect before Timeout to", &serverSockaddr); 
	  result = connectWithTimeout(mySocket, (struct sockaddr *) &serverSockaddr, sizeof(serverSockaddr));
	  if(result != 0)
	    {
	      fprintf(stderr,"Could not connect.\n");
	      continue;
	    }
	  else
	    {
	      printf("Connected.\n\n");
	      return mySocket;
	    }
	}
      return -1;
    }
}
