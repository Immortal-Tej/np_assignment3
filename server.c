#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <netdb.h>
#include <regex>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <map>
#include <stdexcept>

using namespace std;

class Client {
public:
    int fd;
    string nick;
    string inbuf;
    bool registered;

    Client(int f = -1) : fd(f), registered(false) {}
    void clear() { fd = -1; nick = ""; registered = false; inbuf.clear(); }
};

void flush_stdout() { std::fflush(stdout); }
void flush_stderr() { std::fflush(stderr); }

static bool running = true;
static int listenfd = -1;

void handle_sigint(int) {
    running = false;
    if (listenfd >= 0) close(listenfd);
}

// Function to remove trailing newlines from a string
void chomp(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

bool is_valid_nick(const std::string &s) {
    static const std::regex re("^[A-Za-z0-9_]{1,12}$");
    return std::regex_match(s, re);
}

bool split_hostport(const std::string &src, std::string &host, std::string &port) {
    auto pos = src.rfind(':');
    if (pos == std::string::npos) return false;
    host = src.substr(0, pos);
    port = src.substr(pos + 1);
    return !host.empty() && !port.empty();
}

int create_and_bind(const std::string &host, const std::string &port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(rc) << std::endl;
        flush_stderr();
        return -1;
    }

    int lfd = -1;
    for (auto a = res; a != nullptr; a = a->ai_next) {
        lfd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (lfd < 0) continue;
        int on = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(lfd, a->ai_addr, a->ai_addrlen) == 0) {
            if (listen(lfd, 16) == 0) {
                break; // success
            }
        }
        close(lfd);
        lfd = -1;
    }
    freeaddrinfo(res);
    return lfd;
}

ssize_t recv_into(Client &c) {
    char buf[1024];
    ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
    if (n > 0) c.inbuf.append(buf, n);
    return n;
}

void send_response(int clientFd, const std::string &message) {
    ssize_t n = send(clientFd, message.c_str(), message.size(), 0);
    if (n < 0) {
        perror("send failed");
        close(clientFd);
    }
}

void process_client_data(Client &client, std::vector<Client> &clients) {
    size_t pos;
    while ((pos = client.inbuf.find('\n')) != std::string::npos) {
        std::string line = client.inbuf.substr(0, pos);
        client.inbuf.erase(0, pos + 1);
        chomp(line);

        if (!client.registered) {
            if (line.rfind("NICK ", 0) == 0) {
                std::string nick = line.substr(5);
                if (is_valid_nick(nick)) {
                    client.nick = nick;
                    client.registered = true;
                    send_response(client.fd, "OK\n");
                    std::cout << "Client registered with nickname: " << nick << std::endl;
                } else {
                    send_response(client.fd, "ERROR: Invalid nickname format\n");
                }
            } else {
                send_response(client.fd, "ERROR: NICK command expected\n");
            }
        } else {
            if (line.rfind("MSG ", 0) == 0) {
                std::string message = line.substr(4);
                chomp(message);
                if (message.size() > 255) {
                    send_response(client.fd, "ERROR: Message too long\n");
                } else {
                    std::string full_message = "MSG " + client.nick + " " + message + "\n";
                    for (auto &dst : clients) {
                        if (dst.fd >= 0 && dst.fd != client.fd) {
                            send_response(dst.fd, full_message);
                        }
                    }
                }
            } else {
                send_response(client.fd, "ERROR: Unsupported command\n");
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <bindaddr:port>\n";
        flush_stderr();
        return 1;
    }

    std::string host, port;
    if (!split_hostport(argv[1], host, port)) {
        std::cerr << "Bad bind address\n";
        flush_stderr();
        return 1;
    }

    int listenfd = create_and_bind(host, port);
    if (listenfd < 0) {
        std::cerr << "Failed to bind\n";
        flush_stderr();
        return 1;
    }

    std::cout << "[x] Listening on " << host << ":" << port << "\n";
    flush_stdout();

    std::vector<Client> clients;
    fd_set readfds;
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(listenfd, &readfds);
        int maxfd = listenfd;
        for (auto &client : clients) {
            if (client.fd >= 0) {
                FD_SET(client.fd, &readfds);
                if (client.fd > maxfd) maxfd = client.fd;
            }
        }

        int rc = select(maxfd + 1, &readfds, nullptr, nullptr, nullptr);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // new connection
        if (FD_ISSET(listenfd, &readfds)) {
            struct sockaddr_storage sa;
            socklen_t sl = sizeof(sa);
            int cfd = accept(listenfd, (struct sockaddr*)&sa, &sl);
            if (cfd >= 0) {
                clients.emplace_back(cfd);
                const char *g = "HELLO 1.0\n";
                send(cfd, g, strlen(g), 0);
            }
        }

        // iterate clients
        std::vector<int> to_remove;
        for (size_t i = 0; i < clients.size(); ++i) {
            Client &client = clients[i];
            if (client.fd < 0) continue;
            if (!FD_ISSET(client.fd, &readfds)) continue;
            ssize_t n = recv_into(client);
            if (n == 0) {
                std::cout << "Client " << client.nick << " has disconnected." << std::endl;
                close(client.fd);
                client.fd = -1;
                to_remove.push_back(i);
                continue;
            } else if (n < 0) {
                std::cerr << "Error reading from client " << client.nick << ". Closing connection." << std::endl;
                close(client.fd);
                client.fd = -1;
                to_remove.push_back(i);
                continue;
            } else {
                process_client_data(client, clients);
            }
        }

        // cleanup closed clients (remove entries with fd == -1)
        if (!to_remove.empty()) {
            std::sort(to_remove.begin(), to_remove.end(), std::greater<int>());
            for (int idx : to_remove) {
                if (idx >= 0 && idx < (int)clients.size()) {
                    if (clients[idx].fd >= 0) close(clients[idx].fd);
                    clients.erase(clients.begin() + idx);
                }
            }
        }
    }

    // cleanup all
    for (auto &client : clients) {
        if (client.fd >= 0) close(client.fd);
    }
    if (listenfd >= 0) close(listenfd);
    std::cout << "Server shutting down\n";
    flush_stdout();
    return 0;
}
