#ifndef CLIENT_HTTPS_HPP
#define	CLIENT_HTTPS_HPP

#include "client_http.hpp"
#include <boost/asio/ssl.hpp>

namespace SimpleWeb {
    typedef asio_ns::ssl::stream<asio_ns::ip::tcp::socket> HTTPS;
    
    template<>
    class Client<HTTPS> : public ClientBase<HTTPS> {
    public:
        Client(const std::string& server_port_path, bool verify_certificate=true, 
                const std::string& cert_file=std::string(), const std::string& private_key_file=std::string(), 
                const std::string& verify_file=std::string()) : 
                ClientBase<HTTPS>::ClientBase(server_port_path, 443), context(asio_ns::ssl::context::tlsv12) {
            if(cert_file.size()>0 && private_key_file.size()>0) {
                context.use_certificate_chain_file(cert_file);
                context.use_private_key_file(private_key_file, asio_ns::ssl::context::pem);
            }
            
            if(verify_certificate)
                context.set_verify_callback(asio_ns::ssl::rfc2818_verification(host));
            
            if(verify_file.size()>0)
                context.load_verify_file(verify_file);
            else
                context.set_default_verify_paths();
            
            if(verify_file.size()>0 || verify_certificate)
                context.set_verify_mode(asio_ns::ssl::verify_peer);
            else
                context.set_verify_mode(asio_ns::ssl::verify_none);
        }

    protected:
        asio_ns::ssl::context context;
        
        void connect() {
            if(!socket || !socket->lowest_layer().is_open()) {
                std::unique_ptr<asio_ns::ip::tcp::resolver::query> query;
                if(config.proxy_server.empty())
                    query=std::unique_ptr<asio_ns::ip::tcp::resolver::query>(new asio_ns::ip::tcp::resolver::query(host, std::to_string(port)));
                else {
                    auto proxy_host_port=parse_host_port(config.proxy_server, 8080);
                    query=std::unique_ptr<asio_ns::ip::tcp::resolver::query>(new asio_ns::ip::tcp::resolver::query(proxy_host_port.first, std::to_string(proxy_host_port.second)));
                }
                resolver.async_resolve(*query, [this]
                                       (const error_ns::error_code &ec, asio_ns::ip::tcp::resolver::iterator it){
                    if(!ec) {
                        {
                            std::lock_guard<std::mutex> lock(socket_mutex);
                            socket=std::unique_ptr<HTTPS>(new HTTPS(io_service, context));
                        }
                        
                        auto timer=get_timeout_timer(config.timeout_connect);
                        asio_ns::async_connect(socket->lowest_layer(), it, [this, timer]
                                                   (const error_ns::error_code &ec, asio_ns::ip::tcp::resolver::iterator /*it*/){
                            if(timer)
                                timer->cancel();
                            if(!ec) {
                                asio_ns::ip::tcp::no_delay option(true);
                                this->socket->lowest_layer().set_option(option);
                            }
                            else {
                                std::lock_guard<std::mutex> lock(socket_mutex);
                                this->socket=nullptr;
                                throw error_ns::system_error(ec);
                            }
                        });
                    }
                    else {
                        std::lock_guard<std::mutex> lock(socket_mutex);
                        socket=nullptr;
                        throw error_ns::system_error(ec);
                    }
                });
                io_service.reset();
                io_service.run();
                
                if(!config.proxy_server.empty()) {
                    asio_ns::streambuf write_buffer;
                    std::ostream write_stream(&write_buffer);
                    auto host_port=host+':'+std::to_string(port);
                    write_stream << "CONNECT "+host_port+" HTTP/1.1\r\n" << "Host: " << host_port << "\r\n\r\n";
                    auto timer=get_timeout_timer();
                    asio_ns::async_write(socket->next_layer(), write_buffer,
                                             [this, timer](const error_ns::error_code &ec, size_t /*bytes_transferred*/) {
                        if(timer)
                            timer->cancel();
                        if(ec) {
                            std::lock_guard<std::mutex> lock(socket_mutex);
                            socket=nullptr;
                            throw error_ns::system_error(ec);
                        }
                    });
                    io_service.reset();
                    io_service.run();
                    
                    std::shared_ptr<Response> response(new Response());
                    timer=get_timeout_timer();
                    asio_ns::async_read_until(socket->next_layer(), response->content_buffer, "\r\n\r\n",
                                                  [this, timer](const error_ns::error_code& ec, size_t /*bytes_transferred*/) {
                        if(timer)
                            timer->cancel();
                        if(ec) {
                            std::lock_guard<std::mutex> lock(socket_mutex);
                            socket=nullptr;
                            throw error_ns::system_error(ec);
                        }
                    });
                    io_service.reset();
                    io_service.run();
                    parse_response_header(response);
                    if (response->status_code.empty() || response->status_code.compare(0, 3, "200") != 0) {
                        std::lock_guard<std::mutex> lock(socket_mutex);
                        socket=nullptr;
                        throw merror_ns::make_error_code(error_ns::errc::permission_denied);
                    }
                }
                
                auto timer=get_timeout_timer();
                this->socket->async_handshake(asio_ns::ssl::stream_base::client,
                                              [this, timer](const error_ns::error_code& ec) {
                    if(timer)
                        timer->cancel();
                    if(ec) {
                        std::lock_guard<std::mutex> lock(socket_mutex);
                        socket=nullptr;
                        throw error_ns::system_error(ec);
                    }
                });
                io_service.reset();
                io_service.run();
            }
        }
    };
}

#endif	/* CLIENT_HTTPS_HPP */
