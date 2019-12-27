#pragma once
#include <iostream>
#include <asio.hpp>
#include <memory>
#include <vector>
#include "http_parser.hpp"
#include "string_view.hpp"
#include "http_handler.hpp"
#include "http_router.hpp"
#include "mime.hpp"
#include "uuid.hpp"

namespace xfinal {
	class connection final:public std::enable_shared_from_this<connection> {
	public:
		connection(asio::io_service& io, http_router& router, std::string const& static_path, std::string const& upload_path) :socket_(std::unique_ptr<asio::ip::tcp::socket>(new asio::ip::tcp::socket(io))), io_service_(io), buffers_(expand_buffer_size), router_(router), static_path_(static_path), req_(res_), res_(req_), upload_path_(upload_path) {
			left_buffer_size_ = buffers_.size();
		}
	public:
		asio::ip::tcp::socket& get_socket() {
			return *socket_;
		}
	public:
		std::vector<char>& get_buffers() {
			return buffers_;
		}
		bool expand_size(bool is_multipart_data) {
			expand_buffer_size += expand_buffer_size;
			if (expand_buffer_size > max_buffer_size_) {
				if (!is_multipart_data) {
					left_buffer_size_ = 0;
					return false;
				}
				else { //����multipart ���ֿ�����Ҫ��ȡ�������ݵ� ��Ҫ���⴦�� ��Ϊ����һ���Ǵ���mutltipart�����ݲ��ֵ� ֮ǰ�����Ѿ�����¼�� ���������˿������
					expand_buffer_size = max_buffer_size_;
					if (buffers_.size() < expand_buffer_size) {
						buffers_.resize(max_buffer_size_);
					}
					left_buffer_size_ = max_buffer_size_ - current_use_pos_;
					//current_use_pos_ = 0;
					return true;
				}
			}
			buffers_.resize(expand_buffer_size);
			left_buffer_size_ = expand_buffer_size - current_use_pos_;
			return true;
		}
		void set_current_pos(std::size_t addpos) {
			current_use_pos_ += addpos;
		}
		void post_router() {
			router_.post_router(req_, res_);
			write();  //��Ӧ����
		}
		void parse_header(http_parser_header& parser) {  //��������ͷ
			auto request = parser.parse_request_header();
			if (request.first) {
				request_info = std::move(request.second);
				req_.method_ = request_info.method_;
				req_.url_ = request_info.url_;
				req_.version_ = request_info.version_;
				req_.headers_ = &(request_info.headers_);
				req_.header_length_ = parser.get_header_size();
				req_.multipart_form_map_ = &(request_info.multipart_form_map_);
				req_.multipart_files_map_ = &(request_info.multipart_files_map_);
				req_.oct_steam_ = &(request_info.oct_steam_);
				handle_read();
				return;
			}
			request_error("bad request,request header error");
			//http request error;
		}
		void handle_read() {  //��ȡ����ͷ��Ϻ� ��ʼ��һ������
			req_.init_content_type();
			if (req_.has_body()) {  //has request body
				auto type = req_.content_type();
				switch (type)
				{
				case xfinal::content_type::url_encode_form:
				case xfinal::content_type::string:
				{
					pre_process_body(type);
				}
				break;
				case xfinal::content_type::multipart_form:
				{
					pre_process_multipart_body(type);
				}
				break;
				case xfinal::content_type::octet_stream:
				{
					pre_process_oct_stream();
				}
				break;
				case xfinal::content_type::unknow:
				{
					request_error("unsupport content type request");
					return;
				}
				break;
				default:
				{
					return;
				}
				break;
				}
			}
			else {  //���û��body���� Ҳ�п�����websocket
				auto& wss = router_.websokcets();
				if (wss.is_websocket(req_)) {  //��websokcet����
					handle_websocket();  //ȥ����websocket
				}
				else {  //��ͨhttp����
					post_router();  
				}
			}
		}

		void handle_body(content_type type, std::size_t body_length) {  //��ȡ������body�� ����content_type ���д���
			switch (type)
			{
			case content_type::url_encode_form:
			{
				process_url_form(body_length);
			}
			break;
			case content_type::multipart_form:
			{
				process_multipart_form();
			}
			break;
			case content_type::octet_stream:
			{
				process_oct();
			}
				break;
			case content_type::string:
			{
				process_string();
			}
			break;
			default:
				break;
			}
		}
		/// ������content-length��body  ��ȡ����body����  ---start
		void pre_process_body(content_type type) {  //��body ��Ԥ����
			auto header_size = req_.header_length_;
			if (current_use_pos_ > header_size) {  //�ǲ��Ƕ�ͷ��ʱ�� ��bodyҲ��ȡ������ 
				forward_contain_data(buffers_, header_size, current_use_pos_);
				current_use_pos_ -= header_size;
				left_buffer_size_ = buffers_.size() - current_use_pos_;
				auto body_length = req_.body_length();
				if (body_length > current_use_pos_) {    //bodyû�ж�����
					continue_read_data([handler = this->shared_from_this(), type]() {
						handler->process_body(type);
					});
				}
				else {  //��ʼ����content_type �������� ��ִ��·��
					request_info.body_ = std::string(buffers_.data(), body_length);
					handle_body(type, body_length);
					post_router();  //������� ִ��·��
				}
			}
			else {  //����պ������Ĵ�С��ȡ��������headͷ
				buffers_.clear();
				buffers_.resize(1024);
				current_use_pos_ = 0;
				left_buffer_size_ = buffers_.size();
				continue_read_data([handler = this->shared_from_this(), type]() {
					handler->process_body(type);
				}, false);
			}
		}
		void process_body(content_type type) {  //���Ԥ��ȡbody��ʱ��û�ж�����body 
			auto body_length = req_.body_length();
			if (body_length > current_use_pos_) {  //bodyû�ж�����
				continue_read_data([handler = this->shared_from_this(), type]() {
					handler->process_body(type);
				});
			}
			else { //��ʼ����content_type �������� ��ִ��·��
				request_info.body_ = std::string(buffers_.data(), body_length);
				handle_body(type, body_length);
				post_router(); //������� ִ��·��
			}
		}
		/// ������content-length��body  ��ȡ����body���� ---end

		void process_url_form(std::size_t body_length) { // ����application/x-www-form-urlencoded ��������
			request_info.decode_body_ = request_info.body_;
			req_.body_ = request_info.body_;
			http_urlform_parser parser{ request_info.decode_body_ };
			parser.parse_data(request_info.form_map_);
			req_.form_map_ = request_info.form_map_;
		}

		void process_string() {  //�������� json text �ȴ��ı����͵�body
			req_.body_ = nonstd::string_view(request_info.body_);
		}

		void process_multipart_form() {
			post_router();
		}
		////����content_type Ϊmultipart_form ��body����  --start
		void pre_process_multipart_body(content_type type) { //Ԥ����content_type λmultipart_form ���͵�body
			auto header_size = req_.header_length_;
			if (current_use_pos_ > header_size) {  //�ǲ��Ƕ�requestͷ��ʱ�� ��bodyҲ��ȡ������ 
				start_read_pos_ = header_size;
				left_buffer_size_ = buffers_.size() - current_use_pos_;
			}
			else {
				left_buffer_size_ = buffers_.size();
				current_use_pos_ = 0;
				start_read_pos_ = 0;
			}
			process_multipart_body(type);
		}

		void process_multipart_body(content_type type) {
			std::string boundary_start = view2str(req_.boundary_key_);
			if (!boundary_start.empty()) {  //��boundary 
				auto boundary_start_ = "--" + boundary_start;
				auto boundary_end_ = boundary_start_ + std::string("--");
				http_multipart_parser parser{ boundary_start_,boundary_end_ };
				process_mutipart_head(parser, type);
			}
			else {  //���content_typeû��boundary ���Ǵ���
				request_error("bad request,multipart form error");
			}
		}

		void process_mutipart_head(http_multipart_parser const& parser, content_type type) {
			auto end = buffers_.begin() + current_use_pos_;
			//std::cout << nonstd::string_view{ buffers_.data() + start_read_pos_, current_use_pos_ - start_read_pos_ } << std::endl;
			if (parser.is_complete_part_header(buffers_.begin()+ start_read_pos_, end)) {  //�����ǰbuffer���������Ĳ���multipartͷ
				auto pr = parser.parser_part_head(buffers_.begin()+ start_read_pos_, end);//����multipart partͷ�� 
				pre_process_multipart_data(pr, parser); //�������ݲ���
			}
			else {
				continue_read_data([type, parser, handler = this->shared_from_this()]() {
					handler->process_mutipart_head(parser, type);
				}, true, false);  //ֻ�Ƕ�multipart��ͷ ����Ҫ��bufffer���������⴦�� �����ͽ�����
			}
		}

		void pre_process_multipart_data(std::pair<std::size_t, std::map<std::string, std::string>> const& pr, http_multipart_parser const& parser) { //�Ƿ���Ҫ���˵�mutipart head
			if (pr.first != 0) {
				if (current_use_pos_ > pr.first) {  //��ȡmultipart head��ʱ�� Ҳ��ȡ�˲������� �������ݵ�ʱ�� ��֮ǰ��ͷ���Ƴ���
					start_read_pos_ += pr.first;
					left_buffer_size_ = buffers_.size() - current_use_pos_;
					process_multipart_data(pr.second, parser);
				}
				else {  //��multipart head��ʱ��û�ж�ȡ��multipart��data���� �Ϳ�������� ��Ϊ֮ǰ��ͷ�Ѿ���������
					buffers_.clear();
					buffers_.resize(1024);
					current_use_pos_ = 0;
					left_buffer_size_ = 1024;
					start_read_pos_ = 0;
					auto head = pr.second;
					continue_read_data([parser, head, handler = this->shared_from_this()]() {
						handler->process_multipart_data(head, parser);
					}, false, true);
				}
			}
		}

		void process_multipart_data(std::map<std::string, std::string> const& head, http_multipart_parser const& parser) {
			auto dpr = parser.is_complete_part_data(buffers_.data()+start_read_pos_, current_use_pos_ - start_read_pos_);
			if (dpr.first) { //����Ѿ���������������("\r\n--boundary_char")
				record_mutlipart_data(head, nonstd::string_view(buffers_.data()+ start_read_pos_, dpr.second), parser, true);
			}
			else {  //�˴�Ӧ��û������boundary�Ǻ�(��������е����� û�д��� boundary �߽�����,����˵['\r\n--boundary_char']������,Ŀǰʹ��û�г�������)
				record_mutlipart_data(head, nonstd::string_view(buffers_.data()+ start_read_pos_, current_use_pos_ - start_read_pos_), parser, false);
			}
		}

		void record_mutlipart_data(std::map<std::string, std::string> const& head, nonstd::string_view data, http_multipart_parser const& parser, bool is_complete) {
			auto value = head.at("content-disposition");
			auto pos_name = value.find("name") + sizeof("name") + 1;
			auto name = value.substr(pos_name, value.find('\"', pos_name) - pos_name);
			auto type = value.find("filename");
			if (type != std::string::npos) {  //���ļ�����
				auto file = request_info.multipart_files_map_.find(name);
				if (file != request_info.multipart_files_map_.end()) {
					file->second.add_data(data);
				}
				else {
					auto f_type = head.at("content-type");
					auto extension = get_extension(f_type);
					auto t = std::time(nullptr);
					auto& fileo = request_info.multipart_files_map_[name];
					auto path = upload_path_ + "/" + uuids::uuid_system_generator{}().to_short_str() + view2str(extension);
					fileo.open(path);
					auto filename_start = type + sizeof("filename") + 1;
					auto original_name = value.substr(filename_start, value.find('\"', filename_start) - filename_start);
					fileo.set_original_name(std::move(original_name));
					fileo.add_data(data);
				}
			}
			else {  //���ı�����
				request_info.multipart_form_map_[name] += view2str(data);
			}
			if (is_complete) {
				if (type != std::string::npos) {  //������ļ� ��ȡ���� �Ϳ��Թر��ļ�ָ����
					request_info.multipart_files_map_[name].close();
				}
				if (parser.is_end((buffers_.begin()+ start_read_pos_ + data.size() + 2), buffers_.begin() + current_use_pos_)) { //�ж��Ƿ�ȫ��������� �ص�multipart ·��
					handle_body(content_type::multipart_form, 0);
					return;
				}
				//forward_contain_data(buffers_, data.size(), current_use_pos_);  //����һ����ͷ�������ƶ���buffer��ǰ����
				start_read_pos_ += data.size();
				//current_use_pos_ -= data.size();
				left_buffer_size_ = buffers_.size() - current_use_pos_;
				if (left_buffer_size_ == 0) {  //buffers�Ѿ�û�пռ��� ��Ҫ���Ѿ���������ݸ����ˣ��ڳ��ռ�
					forward_contain_data(buffers_, start_read_pos_, current_use_pos_);
					current_use_pos_ -= start_read_pos_;
					start_read_pos_ = 0;
					left_buffer_size_ = buffers_.size() - current_use_pos_;
				}
				process_mutipart_head(parser, content_type::multipart_form);
			}
			else {  //buffers�����е����ݶ���mulitpart ������ ��������multipartͷ�����Կ������
				buffers_.clear();
				start_read_pos_ = 0;
				current_use_pos_ = 0;
				continue_read_data([handler = this->shared_from_this(), head, parser]() {
					handler->process_multipart_data(head, parser);
				}, true, true);
			}
		}
		////����content_type Ϊmultipart_form ��body����  --end

		////����content_type Ϊoct-stream ��body����  --start
		void pre_process_oct_stream() {
			auto body_size = std::atoll(req_.header("content-length").data());
			auto header_size = req_.header_length_;
			if (current_use_pos_ > header_size) {  //�ǲ��Ƕ�requestͷ��ʱ�� ��bodyҲ��ȡ������ 
				request_info.oct_steam_.open(upload_path_ + "/" + uuids::uuid_system_generator{}().to_short_str());
				request_info.oct_steam_.add_data(nonstd::string_view{ &(buffers_[header_size]),current_use_pos_ - header_size });
				if ((current_use_pos_ - header_size) > body_size) {  //����Ѿ���������������
					handle_body(content_type::octet_stream, (std::size_t)body_size);
					return;
				}
			}
			left_buffer_size_ = buffers_.size();
			buffers_.clear();
			current_use_pos_ = 0;
			process_oct_stream(body_size);
		}

		void process_oct_stream(std::uint64_t body_size) {
			auto size = request_info.oct_steam_.size();
			if (request_info.oct_steam_.size() == body_size) {
				request_info.oct_steam_.close();
				handle_body(content_type::octet_stream, (std::size_t)body_size);
				return;
			}
			expand_size(true);
			socket_->async_read_some(asio::buffer(&buffers_[0], buffers_.size()), [handler = this->shared_from_this(), body_size](std::error_code const& ec, std::size_t read_size) {
				if (ec) {
					return;
				}
				handler->request_info.oct_steam_.add_data(nonstd::string_view{ &(handler->buffers_[0]) ,read_size });
				handler->process_oct_stream(body_size);
			});
		}

		void process_oct() {
			post_router();
		}
		////����content_type Ϊoct-stream ��body����  --end

		void handle_websocket() {
			router_.websokcets().update_to_websocket(res_);
			forward_write(true);
			socket_close_ = true; //����connection��˵ sokcet �߼��ر��� ת����websocket����
			auto ws = router_.websokcets().start_webscoket(view2str(req_.url()));
			ws->move_socket(std::move(socket_));
		}

	public:
		template<typename Function>
		void continue_read_data(Function&& callback, bool is_expand = true, bool is_multipart = false) { //����������ȡbodyʣ�����ݵ�
			if (is_expand) {
				bool b = expand_size(is_multipart);
				if (!b) {
					return;
				}
			}
			socket_->async_read_some(asio::buffer(&buffers_[current_use_pos_], left_buffer_size_), [handler = this->shared_from_this(), function = std::move(callback)](std::error_code const& ec, std::size_t read_size) {
				if (ec) {
					return;
				}
				handler->set_current_pos(read_size);
				function();
			});
		}
		void read_header() {  //����������� ��ʼ��ȡ����ͷ
			socket_->async_read_some(asio::buffer(&buffers_[current_use_pos_], left_buffer_size_), [handler = this->shared_from_this()](std::error_code const& ec, std::size_t read_size){
				if (ec) {
					return;
				}
				handler->set_current_pos(read_size);
				auto& buffer = handler->get_buffers();
				//std::cout << handler->get_buffers().data() << std::endl;
				http_parser_header parser{ buffer.begin(),buffer.end() };
				if (parser.is_complete_header().first == parse_state::valid) {
					if (parser.is_complete_header().second) {
						handler->parse_header(parser);
					}
					else {
						handler->expand_size(false);
						handler->read_header();
					}
				}
			});
		}

		void request_error(std::string&& what_error) {
			router_.trigger_error(xfinal_exception{ static_cast<std::string const&>(what_error) });
			res_.write_string(std::move(what_error), false, http_status::bad_request);
			write();
		}

		void write() {
			auto is_chunked = res_.is_chunked_;
			if (!is_chunked) {  //��chunked��ʽ��������
				forward_write();
			}
			else {
				chunked_write();
			}
		}
		void forward_write(bool is_websokcet = false) {  //ֱ��д ��chunked
			if (!socket_close_) {
				asio::async_write(*socket_,res_.to_buffers(), [handler = this->shared_from_this(), is_websokcet](std::error_code const& ec, std::size_t write_size) {
					if (!is_websokcet) {
						handler->close();
					}
				});
			}
		}

		void chunked_write() {
			if (!socket_close_) {
				asio::async_write(*socket_,res_.header_to_buffer(), [handler = this->shared_from_this(), startpos = res_.init_start_pos_](std::error_code const& ec, std::size_t write_size) {
					if (ec) {
						return;
					}
					handler->write_body_chunked(startpos);
				});
			}
		}

		void write_body_chunked(std::int64_t startpos) {
			auto tp = res_.chunked_body(startpos);
			if (!socket_close_) {
				asio::async_write(*socket_, std::get<1>(tp), [handler = this->shared_from_this(), pos = std::get<2>(tp), eof = std::get<0>(tp)](std::error_code const& ec, std::size_t write_size) {
					if (ec) {
						return;
					}
					if (!eof) {
						handler->write_body_chunked(pos);
					}
					else {
						handler->write_end_chunked();
					}
				});
			}
		}

		void write_end_chunked() {
			std::vector<asio::const_buffer> buffers;
			res_.chunked_write_size_ = to_hex(0);
			buffers.emplace_back(asio::buffer(res_.chunked_write_size_));
			buffers.emplace_back(asio::buffer(crlf.data(), crlf.size()));
			buffers.emplace_back(asio::buffer(crlf.data(), crlf.size()));
			if (!socket_close_) {
				asio::async_write(*socket_, buffers, [handler = this->shared_from_this()](std::error_code const& ec, std::size_t write_size) {
					handler->close();
				});
			}
		}
	public:
		void close() {  //��Ӧ��� ׼���ر�����
			if (req_.is_keep_alive()) {
				reset();
				read_header();
				return;
			}
			std::error_code ignore_write_ec;
			socket_->write_some(asio::buffer("\0\0"), ignore_write_ec);  //solve time_wait problem
			std::error_code ignore_shutdown_ec;
			socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ignore_shutdown_ec);
			std::error_code ignore_close_ec;
			socket_->close(ignore_close_ec);
			socket_close_ = true;
		}
	private:
		void reset() {
			req_.reset();
			res_.reset();
			request_info.reset();
			buffers_.clear();
			expand_buffer_size = 1024;
			left_buffer_size_ = 1024;
			current_use_pos_ = 0;
			start_read_pos_ = 0;
			buffers_.resize(expand_buffer_size);
		}
	public:
		void set_chunked_size(std::uint64_t size) {
			res_.chunked_size_ = size;
		}
	public:
		~connection() {
			if (!socket_close_) {
				std::error_code ignore_write_ec;
				socket_->write_some(asio::buffer("\0\0"), ignore_write_ec);  //solve time_wait problem
				std::error_code ignore_shutdown_ec;
				socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ignore_shutdown_ec);
				std::error_code ignore_close_ec;
				socket_->close(ignore_close_ec);
			}
		}
	private:
		std::unique_ptr<asio::ip::tcp::socket> socket_;
		asio::io_service& io_service_;
		std::size_t expand_buffer_size = 1024;
		const std::size_t max_buffer_size_ = 3 * 1024 * 1024;
		std::vector<char> buffers_;
		std::size_t current_use_pos_ = 0;
		std::size_t left_buffer_size_;
		xfinal::request_meta request_info;
		request req_;
		response res_;
		http_router& router_;
		bool socket_close_ = false;
		std::string const& static_path_;
		std::string const& upload_path_;
		std::size_t start_read_pos_ = 0;
	};
}