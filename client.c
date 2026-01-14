#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <chrono>
#include <netdb.h>
#include <regex>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>

using namespace std;

class NetworkClient {
public:
    NetworkClient(const string& address, const string& nickname);
    ~NetworkClient();
    void startCommunication();

private:
    bool isNicknameValid(const string& nickname);
    bool splitHostPort(const string& address, string& host, string& port);
    int createSocketConnection();
    void sendNicknameToServer();
    void receiveServerMessages();
    void sendMessage(const string& message);
    void handleError(const string& errorMsg);
    void gracefulShutdown();

    string serverHost;
    string serverPort;
    string userNickname;
    int socketDescriptor;
    bool nicknameSent;
};

NetworkClient::NetworkClient(const string& address, const string& nickname)
    : userNickname(nickname), nicknameSent(false), socketDescriptor(-1) {
    if (!splitHostPort(address, serverHost, serverPort)) {
        throw runtime_error("Invalid host:port format.");
    }
}

NetworkClient::~NetworkClient() {
    if (socketDescriptor != -1) {
        close(socketDescriptor);
    }
}

bool NetworkClient::isNicknameValid(const string& nickname) {
    if (nickname.length() > 12) {
        cerr << "ERROR: Nickname must be 12 characters or less.\n";
        return false;
    }
    static const std::regex validPattern("^[A-Za-z0-9_]{1,12}$");
    return regex_match(nickname, validPattern);
}

bool NetworkClient::splitHostPort(const string& address, string& host, string& port) {
    size_t colonPos = address.find(':');
    if (colonPos == string::npos) {
        cerr << "ERROR: Missing port in host:port format.\n";
        return false;
    }
    host = address.substr(0, colonPos);
    port = address.substr(colonPos + 1);
    if (host.empty() || port.empty()) {
        cerr << "ERROR: Invalid host or port.\n";
        return false;
    }
    return true;
}

int NetworkClient::createSocketConnection() {
    struct addrinfo hints {}, *res, *ptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(serverHost.c_str(), serverPort.c_str(), &hints, &res) != 0) {
        cerr << "ERROR: Could not resolve host\n";
        return -1;
    }

    int socketFD = -1;
    for (ptr = res; ptr; ptr = ptr->ai_next) {
        socketFD = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (socketFD < 0) continue;

        if (connect(socketFD, ptr->ai_addr, ptr->ai_addrlen) == 0) break;

        close(socketFD);
        socketFD = -1;
    }

    freeaddrinfo(res);
    return socketFD;
}

void NetworkClient::sendNicknameToServer() {
    string nicknameCommand = "NICK " + userNickname + "\n";
    ssize_t bytesSent = send(socketDescriptor, nicknameCommand.c_str(), nicknameCommand.size(), 0);
    if (bytesSent < 0) {
        handleError("Failed to send nickname to server.");
    }
}

void NetworkClient::receiveServerMessages() {
    fd_set readFds;
    char buffer[2048];
    string messageBuffer;
    string serverLine;

    while (true) {
        FD_ZERO(&readFds);
        FD_SET(socketDescriptor, &readFds);
        FD_SET(STDIN_FILENO, &readFds);

        int maxFd = max(socketDescriptor, STDIN_FILENO);
        int selectResult = select(maxFd + 1, &readFds, nullptr, nullptr, nullptr);
        if (selectResult < 0) {
            if (errno == EINTR) continue;
            handleError("select failed during receiving server messages.");
        }

        if (FD_ISSET(socketDescriptor, &readFds)) {
            ssize_t bytesReceived = recv(socketDescriptor, buffer, sizeof(buffer) - 1, 0);
            if (bytesReceived < 0) {
                handleError("Failed to receive data from server.");
            } else if (bytesReceived == 0) {
                cout << "Connection closed by server.\n";
                return;
            }

            buffer[bytesReceived] = '\0';  // Null-terminate the received data
            messageBuffer.append(buffer, bytesReceived);

            // Process each complete line from the buffer
            size_t newlinePos;
            while ((newlinePos = messageBuffer.find('\n')) != string::npos) {
                string line = messageBuffer.substr(0, newlinePos + 1);
                messageBuffer.erase(0, newlinePos + 1);

                // If the line contains the message identifier "MSG ", process it
                if (line.find("MSG ") == 0) {
                    cout << line.substr(4);  // Print the message part after "MSG "
                } else {
                    cout << line;  // Print the full line
                }
                cout.flush();
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readFds)) {
            string userMessage;
            if (!getline(cin, userMessage)) break;

            if (userMessage.size() > 255) {
                cerr << "ERROR: Message too long. Max 255 characters.\n";
                continue;
            }

            sendMessage(userMessage);
        }
    }
}

void NetworkClient::sendMessage(const string& message) {
    string messageToSend = "MSG " + message + "\n";
    ssize_t bytesSent = send(socketDescriptor, messageToSend.c_str(), messageToSend.size(), 0);
    if (bytesSent < 0) {
        handleError("Failed to send message.");
    }
}

void NetworkClient::handleError(const string& errorMsg) {
    cerr << errorMsg << endl;
    gracefulShutdown();
    throw runtime_error(errorMsg);
}

void NetworkClient::gracefulShutdown() {
    if (socketDescriptor != -1) {
        close(socketDescriptor);
    }
}

void NetworkClient::startCommunication() {
    socketDescriptor = createSocketConnection();
    if (socketDescriptor < 0) {
        cerr << "ERROR: Failed to connect to server." << endl;
        return;
    }

    cout << "Connected to server at " << serverHost << ":" << serverPort << endl;

    string greetingBuffer;
    char tempBuffer[2048];
    bool greetingReceived = false;
    auto start = chrono::steady_clock::now();

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(socketDescriptor, &rfds);
        struct timeval timeout{3, 0};
        int selectResult = select(socketDescriptor + 1, &rfds, nullptr, nullptr, &timeout);
        if (selectResult > 0 && FD_ISSET(socketDescriptor, &rfds)) {
            ssize_t bytesReceived = recv(socketDescriptor, tempBuffer, sizeof(tempBuffer) - 1, 0);
            if (bytesReceived <= 0) break;
            tempBuffer[bytesReceived] = '\0';
            greetingBuffer.append(tempBuffer, bytesReceived);

            if (greetingBuffer.find("HELLO 1") != string::npos || greetingBuffer.find("HELLO 1.0") != string::npos) {
                greetingReceived = true;
                break;
            }
        }

        auto now = chrono::steady_clock::now();
        if (chrono::duration_cast<chrono::seconds>(now - start).count() > 5) break;
    }

    if (!greetingReceived) {
        handleError("No HELLO received from server.");
    }

    sendNicknameToServer();
    cout << "Nickname sent successfully. Handshake complete." << endl;

    receiveServerMessages();
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <host:port> <nickname>\n";
        return 1;
    }

    string serverAddress = argv[1];
    string nickname = argv[2];

    try {
        NetworkClient client(serverAddress, nickname);
        client.startCommunication();
    } catch (const runtime_error& e) {
        cerr << e.what() << endl;
        return 1;
    }

    return 0;
}
