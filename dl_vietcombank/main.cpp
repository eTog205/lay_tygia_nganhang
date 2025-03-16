
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <sql.h>
#include <sqlext.h>
#include <string>
#include <vector>
#pragma comment(lib, "odbc32.lib")

using namespace std;
namespace http = boost::beast::http;

#define SQL_SERVER "localhost"
#define SQL_DATABASE "xd"
#define SQL_USER "sa"
#define SQL_PASSWORD "XDXDxdxd123456@"

struct ExchangeRate
{
	string currency_name;
	string currency_code;
	string cash_buy;
	string transfer_buy;
	string sell;
};

// Hàm lấy HTML từ Vietcombank bằng HTTPS
string GetExchangeRateData()
{
	try
	{
		boost::asio::io_context ioc;
		boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv12_client);
		ctx.set_options(boost::asio::ssl::context::no_tlsv1 | boost::asio::ssl::context::no_tlsv1_1);
		ctx.set_default_verify_paths();

		boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(ioc, ctx);
		boost::asio::ip::tcp::resolver resolver(ioc);
		auto const results = resolver.resolve("portal.vietcombank.com.vn", "443");
		boost::asio::connect(stream.next_layer(), results.begin(), results.end());

		if (!SSL_set_tlsext_host_name(stream.native_handle(), "portal.vietcombank.com.vn"))
		{
			boost::system::error_code ec{ static_cast<int>(::ERR_get_error()),
										  boost::asio::error::get_ssl_category() };
			throw boost::system::system_error{ ec };
		}

		// Không dùng xác thực chứng chỉ (chỉ dùng cho thử nghiệm)
		stream.set_verify_mode(boost::asio::ssl::verify_none);
		stream.handshake(boost::asio::ssl::stream_base::client);

		http::request<http::string_body> req(http::verb::get, "/UserControls/TVPortal.TyGia/pListTyGia.aspx", 11);
		req.set(http::field::host, "portal.vietcombank.com.vn");
		req.set(http::field::user_agent, "Boost.Beast");
		http::write(stream, req);

		boost::beast::flat_buffer buffer;
		http::response<http::dynamic_body> res;
		http::read(stream, buffer, res);

		return buffers_to_string(res.body().data());
	} catch (exception& e)
	{
		cerr << "Lỗi HTTP: " << e.what() << endl;
		return "";
	}
}

// Hàm loại bỏ khoảng trắng đầu/cuối
string trim(const string& str)
{
	size_t dau = str.find_first_not_of(" \t\n\r");
	if (dau == string::npos)
		return "";
	size_t cuoi = str.find_last_not_of(" \t\n\r");
	return str.substr(dau, (cuoi - dau + 1));
}

// Hàm xử lý giá trị (cắt bỏ khoảng trắng, ...)
string cleanValue(const string& giatri)
{
	return trim(giatri);
}

// Hàm loại bỏ dấu phẩy khỏi chuỗi số
string RemoveCommas(const string& str)
{
	string kq = str;
	kq.erase(ranges::remove(kq, ',').begin(), kq.end());
	return kq;
}

// Hàm lọc dữ liệu tỷ giá từ nội dung HTML (đọc trực tiếp từ bộ nhớ) sử dụng libxml2 và XPath
vector<ExchangeRate> parseHTMLFromString(const string& nd_html)
{
	vector<ExchangeRate> rates;

	htmlDocPtr doc = htmlReadMemory(nd_html.c_str(), static_cast<int>(nd_html.size()), nullptr, nullptr, HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
	if (doc == nullptr)
	{
		cerr << "Lỗi: Không thể parse HTML content!" << endl;
		return rates;
	}

	xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
	if (xpathCtx == nullptr)
	{
		cerr << "Lỗi: Không thể tạo XPath context" << endl;
		xmlFreeDoc(doc);
		return rates;
	}

	// XPath query: lấy tất cả các hàng <tr> trong bảng có id 'ctl00_Content_ExrateView'
	xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>("//table[@id='ctl00_Content_ExrateView']//tr"), xpathCtx);
	if (xpathObj == nullptr)
	{
		cerr << "Lỗi: Không thể thực hiện XPath query" << endl;
		xmlXPathFreeContext(xpathCtx);
		xmlFreeDoc(doc);
		return rates;
	}

	xmlNodeSetPtr nut = xpathObj->nodesetval;
	int kichthuoc = (nut) ? nut->nodeNr : 0;
	for (int i = 0; i < kichthuoc; i++)
	{
		xmlNode* hang = nut->nodeTab[i];
		bool isHeader = false;
		for (xmlNode* cot = hang->children; cot; cot = cot->next)
		{
			if (cot->type == XML_ELEMENT_NODE && xmlStrcmp(cot->name, reinterpret_cast<const xmlChar*>("th")) == 0)
			{
				isHeader = true;
				break;
			}
		}
		if (isHeader)
			continue;

		ExchangeRate rate;
		int cot_vt = 0;
		for (xmlNode* cot = hang->children; cot; cot = cot->next)
		{
			if (cot->type != XML_ELEMENT_NODE || xmlStrcmp(cot->name, reinterpret_cast<const xmlChar*>("td")) != 0)
				continue;
			xmlChar* content = xmlNodeGetContent(cot);
			string value = content ? reinterpret_cast<char*>(content) : "";
			xmlFree(content);
			value = cleanValue(value);
			switch (cot_vt)
			{
				case 0: rate.currency_name = value; break;
				case 1: rate.currency_code = value; break;
				case 2: rate.cash_buy = value; break;
				case 3: rate.transfer_buy = value; break;
				case 4: rate.sell = value; break;
				default: break;
			}
			cot_vt++;
		}
		if (!rate.currency_name.empty() && !rate.currency_code.empty())
			rates.push_back(rate);
	}

	xmlXPathFreeObject(xpathObj);
	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
	return rates;
}

bool UpdateExchangeRate(const string& currency, double mua_tienmat, double mua_chuyenkhoan, double ban)
{
	SQLHENV h_env = nullptr;
	SQLHDBC h_dbc = nullptr;
	SQLHSTMT h_stmt = nullptr;
	SQLRETURN ret;

	// Khởi tạo môi trường ODBC
	ret = SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &h_env);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
		return false;

	ret = SQLSetEnvAttr(h_env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		SQLFreeHandle(SQL_HANDLE_ENV, h_env);
		return false;
	}

	ret = SQLAllocHandle(SQL_HANDLE_DBC, h_env, &h_dbc);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		SQLFreeHandle(SQL_HANDLE_ENV, h_env);
		return false;
	}

	// Chuỗi kết nối tới SQL Server
	string conn_str = "DRIVER={SQL Server};SERVER=" SQL_SERVER ";DATABASE=" SQL_DATABASE ";UID=" SQL_USER ";PWD=" SQL_PASSWORD ";";
	wstring wconn_str(conn_str.begin(), conn_str.end());
	ret = SQLDriverConnectW(h_dbc, nullptr, const_cast<SQLWCHAR*>(wconn_str.c_str()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
		SQLFreeHandle(SQL_HANDLE_ENV, h_env);
		return false;
	}

	// Thực hiện UPDATE
	string update_query = "UPDATE tygia_ngoaite SET mua_tienmat = " + to_string(mua_tienmat) +
		", mua_chuyenkhoan = " + to_string(mua_chuyenkhoan) +
		", ban = " + to_string(ban) +
		" WHERE ma_ngoaite = '" + currency + "';";
	wstring wupdate_query(update_query.begin(), update_query.end());

	ret = SQLAllocHandle(SQL_HANDLE_STMT, h_dbc, &h_stmt);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		SQLDisconnect(h_dbc);
		SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
		SQLFreeHandle(SQL_HANDLE_ENV, h_env);
		return false;
	}

	ret = SQLExecDirectW(h_stmt, const_cast<SQLWCHAR*>(wupdate_query.c_str()), SQL_NTS);
	SQLLEN rowsAffected = 0;
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
	{
		SQLRowCount(h_stmt, &rowsAffected);
	} else
	{
		// Có thể in ra lỗi nếu cần (như trước)
	}
	SQLFreeHandle(SQL_HANDLE_STMT, h_stmt);

	if (rowsAffected == 0)
	{
		// Nếu không có bản ghi nào được update, thực hiện INSERT
		string insert_query = "INSERT INTO tygia_ngoaite (ma_ngoaite, mua_tienmat, mua_chuyenkhoan, ban) VALUES ('" +
			currency + "', " + to_string(mua_tienmat) + ", " +
			to_string(mua_chuyenkhoan) + ", " + to_string(ban) + ");";
		wstring winsert_query(insert_query.begin(), insert_query.end());

		ret = SQLAllocHandle(SQL_HANDLE_STMT, h_dbc, &h_stmt);
		if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
		{
			SQLDisconnect(h_dbc);
			SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
			SQLFreeHandle(SQL_HANDLE_ENV, h_env);
			return false;
		}
		ret = SQLExecDirectW(h_stmt, const_cast<SQLWCHAR*>(winsert_query.c_str()), SQL_NTS);
		if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
		{
			// Nếu cần in lỗi: in ra thông báo lỗi chi tiết bằng SQLGetDiagRecW như đã làm trước.
			SQLFreeHandle(SQL_HANDLE_STMT, h_stmt);
			SQLDisconnect(h_dbc);
			SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
			SQLFreeHandle(SQL_HANDLE_ENV, h_env);
			return false;
		}
		SQLFreeHandle(SQL_HANDLE_STMT, h_stmt);
	}

	// Ngắt kết nối và giải phóng handle
	SQLDisconnect(h_dbc);
	SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
	SQLFreeHandle(SQL_HANDLE_ENV, h_env);
	return true;
}


// Hàm cập nhật dữ liệu vào CSDL và in ra console thông báo cho từng dòng
void updateAndPrintData(const vector<ExchangeRate>& rates)
{
	for (const auto& rate : rates)
	{
		// Loại bỏ dấu phẩy trước khi chuyển đổi giá trị số
		string cashStr = RemoveCommas(rate.cash_buy);
		string transferStr = RemoveCommas(rate.transfer_buy);
		string sellStr = RemoveCommas(rate.sell);

		double cash = (cashStr == "-" || cashStr.empty()) ? 0.0 : std::stod(cashStr);
		double transfer = (transferStr == "-" || transferStr.empty()) ? 0.0 : std::stod(transferStr);
		double sell = (sellStr == "-" || sellStr.empty()) ? 0.0 : std::stod(sellStr);

		cout << "Đang cập nhật: " << rate.currency_code
			<< " | Mua tiền mặt: " << cash
			<< " | Mua chuyển khoản: " << transfer
			<< " | Bán: " << sell << " ... ";

		if (UpdateExchangeRate(rate.currency_code, cash, transfer, sell))
			cout << "Cập nhật thành công!" << endl;
		else
			cerr << "Lỗi cập nhật!" << endl;
	}
}

int main()
{
	string html = GetExchangeRateData();
	if (html.empty())
	{
		cerr << "Lỗi: Không thể lấy dữ liệu từ Vietcombank!" << endl;
		return 1;
	}
	//lọc
	vector<ExchangeRate> rates = parseHTMLFromString(html);
	if (rates.empty())
	{
		cerr << "Lỗi: Không tìm thấy dữ liệu tỷ giá trong HTML!" << endl;
	} else
	{
		// Cập nhật dữ liệu
		updateAndPrintData(rates);
		cout << "Quá trình cập nhật tỷ giá hoàn tất!" << endl;
	}

	return 0;
}
