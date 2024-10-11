/// @author Alexander Rykovanov 2012
/// @email rykovanov.as@gmail.com
/// @brief Opc binary cnnection channel.
/// @license GNU LGPL
///
/// Distributed under the GNU LGPL License
/// (See accompanying file LICENSE or copy at
/// http://www.gnu.org/licenses/lgpl.html)
///

#include <opc/ua/socket_channel.h>
#include <opc/ua/errors.h>



#include <errno.h>
#include <iostream>

#include <stdexcept>
#include <string.h>

#include <sys/types.h>


#ifdef _WIN32
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif


OpcUa::SocketChannel::SocketChannel(int sock, const Common::Logger::SharedPtr& logger)
  : Socket(sock),
    Logger(logger)
{
  int flag = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

  if (Socket < 0)
    {
      LOG_CRITICAL(Logger, "{:90}| --> {}", FUNCTION_LINE_NAME, Socket);
      THROW_ERROR(CannotCreateChannelOnInvalidSocket);
    }
}

OpcUa::SocketChannel::~SocketChannel()
{
  Stop();
}

void OpcUa::SocketChannel::Stop()
{
  //LOG_CRITICAL(Logger, "{:90}| Stop", FUNCTION_LINE_NAME);
#ifdef _WIN32
  closesocket(Socket);
#else
  close(Socket);
#endif
}

std::size_t OpcUa::SocketChannel::Receive(char * data, std::size_t size)
{
    //LOG_CRITICAL(Logger, "{:90}| -->recv {}", FUNCTION_LINE_NAME, size);
    int received = recv(Socket, data, size, MSG_WAITALL);
    //LOG_CRITICAL(Logger, "{:90}| <--recv {}", FUNCTION_LINE_NAME, size);

  if (received < 0)
    {
      LOG_CRITICAL(Logger, "{:90}| Failed to receive data from host. size {}, received {}", FUNCTION_LINE_NAME, size, received);
      THROW_OS_ERROR("Failed to receive data from host.");
    }

  if (received == 0)
    {
      LOG_CRITICAL(Logger, "{:90}| Connection was closed by host. size {}, received {}", FUNCTION_LINE_NAME, size, received);
      THROW_OS_ERROR("Connection was closed by host.");
    }


  return (std::size_t)size;
}

void OpcUa::SocketChannel::Send(const char * message, std::size_t size)
{
    Logger->flush();
    //LOG_WARN(Logger, "{:90}| -->send {}", FUNCTION_LINE_NAME, size);
    int sent = send(Socket, message, size, 0);
    //LOG_WARN(Logger, "{:90}| <--send {}", FUNCTION_LINE_NAME, sent);
                                                                    

  if (sent != (int)size)
    {
      LOG_CRITICAL(Logger, "{:90}| unable to send data to the host. size {}, sent {}", FUNCTION_LINE_NAME, size, sent);
      THROW_OS_ERROR("unable to send data to the host. ");
    }
}
