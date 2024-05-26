#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <string>
#include <sstream>
// #include <math.h>
#include <sys/wait.h>
#include <boost/asio.hpp>
#include <map>

using namespace std;
using boost::asio::ip::tcp;

int state;

struct SOCKS_ {
    int vn;
    int cd;
    string srcIP;
    string srcPort;
    string dstIP;
    string dstPort;
    string ConnectOrBind;
    string reply;
} SOCKS4;

class session : public enable_shared_from_this<session> {
    public : 
        session(tcp::socket socket, boost::asio::io_context& io_context) : socket_Client(move(socket)), socket_Server(io_context), iocontext(io_context), resolver_(io_context){}

        void start() {
            do_read();
        }
    
    private :
        void do_read() {
            // 確保Session存活的時間比 async 操作還要長
            auto self(shared_from_this());
            memset(data_, '\0', sizeof(data_));
            socket_Client.async_read_some(boost::asio::buffer(data_, max_length),
                [this, self](boost::system::error_code ec, size_t length) {
                    if (!ec) {
                        socks4reply = {0, 0, 0, 0, 0, 0, 0, 0};
                        do_parseSocks4Req(length);
                        cout << "<S_IP>: " << socksMsg.srcIP << "\n" << flush;
                        cout << "<S_PORT>: " << socksMsg.srcPort << "\n" << flush;
                        cout << "<D_IP>: " << socksMsg.dstIP << "\n" << flush;
                        cout << "<D_PORT>: " << socksMsg.dstPort << "\n" << flush;
                        cout << "<Command>: " << socksMsg.ConnectOrBind << "\n" << flush;
                        if (socksMsg.reply == "Accept") {
                            socks4reply[1] = 90;
                            if (socksMsg.cd == 1) {
                                do_connectServer();
                            }
                            else if (socksMsg.cd == 2) {
                                do_bindServer();
                            }
                        }
                        else {
                            socks4reply[1] = 91;
                            do_writeReply();
                            // exit(0);
                        }
                    }
                }
            );
        }

        void do_connectServer() {
            auto self(shared_from_this());
            resolver_(iocontext);
			tcp::resolver::results_type ep = resolver_.resolve(socksMsg.dstIP, socksMsg.dstPort);
            boost::asio::async_connect(socket_Server, ep,
                [this, self](boost::system:error_code ec, tcp::endpoint ed) {
                    if (!ec) {
                        do_writeReply();
                        do_readFromClient();
                        do_readFromServer();
                    }
                    else {
                        socks4reply[1] = 91;
                        do_writeReply();
                        socket_Client.close();
                        socket_Server.close();
                    }
                }
            );
        }

        void do_bindServer() {
            // 創建一個tcp::acceptor對象，綁定到一個自動分配的port
            tcp::acceptor acceptor_(iocontext, tcp::endpoint(tcp::v4(), 0));
            acceptor_.listen();
            // 取的被自動分配的port number
            unsigned int port = acceptor_.local_endpoint().port();
            socks4reply = {0, 90, unsigned int(port / 256), unsigned int(port % 256), 0, 0, 0, 0};
            do_writeReply();
            acceptor_.accept(socket_Server);
            do_writeReply();
            do_readFromClient();
            do_readFromServer();
        }

        void do_readFromClient() {
            memset(data_Client, '\0', sizeof(data_Client));
            auto self(shared_from_this());
            socket_Client.async_read_some(boost::asio::buffer(data_Client, max_length), 
				[this, self](boost::system::error_code ec, std::size_t length){
					if (!ec) {
						do_writeToServer(length);
					}
                    else {
						socket_Client.close();
						socket_Server.close();
						// exit(0);
					}
				}
			);
        }

        void do_readFromServer() {
            memset(data_Server, '\0', sizeof(data_Server));
            auto self(shared_from_this());
            socket_Server.async_read_some(boost::asio::buffer(data_Server, max_length), 
				[this, self](boost::system::error_code ec, std::size_t length){
					if (!ec) {
						do_writeToClient(length);
					}
                    else {
						socket_Client.close();
						socket_Server.close();
						// exit(0);
					}
				}
			);
        }

        void do_writeToClient(size_t length) {
            auto self(shared_from_this());
			boost::asio::async_write(socket_Client, boost::asio::buffer(data_Server, length),
				[this, self](boost::system::error_code ec, size_t length){
					if(!ec){
						do_readFromServer();
					}
				}
			);
        }

        void do_writeToServer(size_t length) {
            auto self(shared_from_this());
			boost::asio::async_write(socket_Server, boost::asio::buffer(data_Client, length),
				[this, self](boost::system::error_code ec, size_t length){
					if(!ec){
						do_readFromClient();
					}
				}
			);
        }

        void do_writeReply() {
            auto self(shared_from_this());
            boost::asio::async_write(socket_Client, boost::asio::buffer(socks4reply, 8),
                [this, self](boost::system::error_code ec, size_t length){
                    if(!ec){
                        cerr << "write reply error ! (socks Reject or Failed)" << endl;
                    }
                }
            );
        }

        void do_parseSocks4Req(size_t length) {
            socksMsg.vn = data_[0];
            socksMsg.cd = data_[1];
            socksMsg.cmd = (socksMsg.cd == 1) ? "CONNECT" : "BIND";
            socksMsg.dstPort = to_string((unsigned int) (data_[2] << 8) | (data_[3]));
            if (data_[4] == 0 && data_[5] == 0 && data_[6] == 0 && data_[7] != 0) {
                bool domainNameHead = false;
                string domainName = "";
                for (int i = 8; i < length; i++) {
                    if (data_[i] == 0) {
                        if (domainNameHead) break;
                        domainNameHead = true;
                    }
                    else if (domainNameHead) {
                        domainName.push_back(data_[i]);
                    }
                }
                tcp::endpoint ep = resolver_.resolve(domainName, socksMsg.dstPort)->endpoint();
                socksMsg.dstIP = ep.address().to_string();
            }
            else {
                char ip[20];
				snprintf(ip, 20, "%d.%d.%d.%d", data_[4], data_[5], data_[6], data_[7]);
				socksMsg.dstIP = string(ip);
            }
            socksMsg.srcIP = socket_Client.remote_endpoint().address().to_string();
            socksMsg.srcPort = to_string(socket_Client.remote_endpoint().port());
        }

        tcp::socket socket_Client;
        tcp::socket socket_Server;
        tcp::resolver resolver_;
        boost::asio::io_context& iocontext;
        enum { max_length = 1024 };
        unsigned char data_[max_length];
        unsigned char data_Server[max_length];
        unsigned char data_Client[max_length];
        SOCKS4 socksMsg;
        unsigned char socks4reply[8];
};

class server {
    public : 
        server(boost::asio::io_context& io_context, short port) : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)), iocontext(io_context), signal_set(io_context, SIGCHLD) {
            asyncWait();
            do_accept();
        }
        
    private:
        void asyncWait() {
            signal_set.async_wait(handler);
        }
        void handler(const boost::system::error_code& error, int signal_number) {
            if (!error) {
                if(acceptor_.is_open()){
                    while(waitpid(-1, &state, WNOHANG) > 0);
                    asyncWait();
                }
            }
        }
        void do_accept() {
            acceptor_.async_accept(
                [this](boost::system::error_code ec, tcp::socket socket) {
                    if (!ec) {
                        iocontext.notify_fork(boost::asio::io_context::fork_prepare);
                        pid_t pid;

                        while ((pid = fork()) < 0) {
                            usleep(1000);
                        }
                        if (pid == 0) {
                            iocontext.notify_fork(boost::asio::io_context::fork_child);
                            acceptor_.close();
                            signal_set.cancel();
                            make_shared<session>(move(socket), iocontext)->start();
                        }
                        else if (pid > 0) {
                            iocontext.notify_fork(boost::asio::io_context::fork_parent);
                            socket.close();
                            do_accept();
                        }
                    }
                }
            );
        }

        tcp::acceptor acceptor_;
        boost::asio::signal_set signal_set;
        boost::asio::io_context& iocontext
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            cerr << "Usage: async_tcp_echo_server <port>\n";
            return 1;
        }

        boost::asio::io_context io_context;

        server s(io_context, atoi(argv[1]));
        io_context.run();
    }
    catch (exception& e) {
        cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}