#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <sqlext.h>
#pragma comment(lib, "odbc32.lib")

using namespace std;
namespace http = boost::beast::http;

#define SQL_SERVER "localhost"
#define SQL_DATABASE "xd"
#define SQL_USER "sa"
#define SQL_PASSWORD "XDXDxdxd123456@"

struct ty_gia_ngoai_te
{
	string ten_ngoaite;
	string ma_ngoaite;
	string mua_tienmat;
	string mua_chuyenkhoan;
	string ban;
};

//lấy dl từ vietcombank qua HTTPS
string lay_dl_tygia()
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

		//!! Không xác thực chứng chỉ
		stream.set_verify_mode(boost::asio::ssl::verify_none);
		stream.handshake(boost::asio::ssl::stream_base::client);

		http::request<http::string_body> req(http::verb::get, "/UserControls/TVPortal.TyGia/pListTyGia.aspx", 11);
		req.set(http::field::host, "portal.vietcombank.com.vn");
		req.set(http::field::user_agent, "Boost.Beast");
		http::write(stream, req);

		boost::beast::flat_buffer buffer;
		http::response<http::dynamic_body> res;
		read(stream, buffer, res);

		return buffers_to_string(res.body().data());
	} catch (exception& e)
	{
		cerr << "Lỗi HTTP: " << e.what() << endl;
		return "";
	}
}

string bo_khoangtrang(const string& str)
{
	size_t dau = str.find_first_not_of(" \t\n\r");
	if (dau == string::npos)
		return "";
	size_t cuoi = str.find_last_not_of(" \t\n\r");
	return str.substr(dau, (cuoi - dau + 1));
}

string xl_giatri(const string& giatri)
{
	return bo_khoangtrang(giatri);
}

string bo_dauphay(const string& str)
{
	string kq = str;
	kq.erase(ranges::remove(kq, ',').begin(), kq.end());
	return kq;
}

vector<ty_gia_ngoai_te> loc_dl_html(const string& nd_html)
{
	vector<ty_gia_ngoai_te> ds_tygia;

	htmlDocPtr doc = htmlReadMemory(nd_html.c_str(), static_cast<int>(nd_html.size()), nullptr, nullptr, HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
	if (doc == nullptr)
	{
		cerr << "Lỗi: Không thể parse HTML content!" << endl;
		return ds_tygia;
	}

	xmlXPathContextPtr xpath_ctx = xmlXPathNewContext(doc);
	if (xpath_ctx == nullptr)
	{
		cerr << "Lỗi: Không thể tạo XPath context" << endl;
		xmlFreeDoc(doc);
		return ds_tygia;
	}

	xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>("//table[@id='ctl00_Content_ExrateView']//tr"), xpath_ctx);
	if (xpathObj == nullptr)
	{
		cerr << "Lỗi: Không thể thực hiện XPath query" << endl;
		xmlXPathFreeContext(xpath_ctx);
		xmlFreeDoc(doc);
		return ds_tygia;
	}

	xmlNodeSetPtr nut = xpathObj->nodesetval;
	int kichthuoc = (nut) ? nut->nodeNr : 0;
	for (int i = 0; i < kichthuoc; i++)
	{
		xmlNode* hang = nut->nodeTab[i];
		bool la_hang_tieude = false;
		for (xmlNode* cot = hang->children; cot; cot = cot->next)
		{
			if (cot->type == XML_ELEMENT_NODE && xmlStrcmp(cot->name, reinterpret_cast<const xmlChar*>("th")) == 0)
			{
				la_hang_tieude = true;
				break;
			}
		}
		if (la_hang_tieude)
			continue;

		ty_gia_ngoai_te tygia;
		int vt_cot = 0;
		for (xmlNode* cot = hang->children; cot; cot = cot->next)
		{
			if (cot->type != XML_ELEMENT_NODE || xmlStrcmp(cot->name, reinterpret_cast<const xmlChar*>("td")) != 0)
				continue;
			xmlChar* noidung = xmlNodeGetContent(cot);
			string giatri = noidung ? reinterpret_cast<char*>(noidung) : "";
			xmlFree(noidung);
			giatri = xl_giatri(giatri);
			switch (vt_cot)
			{
				case 0: tygia.ten_ngoaite = giatri; break;
				case 1: tygia.ma_ngoaite = giatri; break;
				case 2: tygia.mua_tienmat = giatri; break;
				case 3: tygia.mua_chuyenkhoan = giatri; break;
				case 4: tygia.ban = giatri; break;
				default: break;
			}
			vt_cot++;
		}
		if (!tygia.ten_ngoaite.empty() && !tygia.ma_ngoaite.empty())
			ds_tygia.push_back(tygia);
	}

	xmlXPathFreeObject(xpathObj);
	xmlXPathFreeContext(xpath_ctx);
	xmlFreeDoc(doc);
	return ds_tygia;
}

bool capnhat_tygia(const string& currency, double mua_tienmat, double mua_chuyenkhoan, double ban)
{
	SQLHENV h_env = nullptr;
	SQLHDBC h_dbc = nullptr;
	SQLHSTMT h_stmt = nullptr;
	SQLRETURN ret;

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

	string conn_str = "DRIVER={SQL Server};SERVER=" SQL_SERVER ";DATABASE=" SQL_DATABASE ";UID=" SQL_USER ";PWD=" SQL_PASSWORD ";";
	wstring wconn_str(conn_str.begin(), conn_str.end());
	ret = SQLDriverConnectW(h_dbc, nullptr, const_cast<SQLWCHAR*>(wconn_str.c_str()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
		SQLFreeHandle(SQL_HANDLE_ENV, h_env);
		return false;
	}

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
	}
	SQLFreeHandle(SQL_HANDLE_STMT, h_stmt);

	if (rowsAffected == 0)
	{
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
			SQLFreeHandle(SQL_HANDLE_STMT, h_stmt);
			SQLDisconnect(h_dbc);
			SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
			SQLFreeHandle(SQL_HANDLE_ENV, h_env);
			return false;
		}
		SQLFreeHandle(SQL_HANDLE_STMT, h_stmt);
	}

	SQLDisconnect(h_dbc);
	SQLFreeHandle(SQL_HANDLE_DBC, h_dbc);
	SQLFreeHandle(SQL_HANDLE_ENV, h_env);
	return true;
}

void capnhat_intb(const vector<ty_gia_ngoai_te>& ds_tygia)
{
	for (const auto& tygia : ds_tygia)
	{
		string strTienMat = bo_dauphay(tygia.mua_tienmat);
		string strChuyenKhoan = bo_dauphay(tygia.mua_chuyenkhoan);
		string strBan = bo_dauphay(tygia.ban);

		double tienmat = (strTienMat == "-" || strTienMat.empty()) ? 0.0 : std::stod(strTienMat);
		double chuyenkhoan = (strChuyenKhoan == "-" || strChuyenKhoan.empty()) ? 0.0 : std::stod(strChuyenKhoan);
		double giaban = (strBan == "-" || strBan.empty()) ? 0.0 : std::stod(strBan);

		cout << "Đang cập nhật: " << tygia.ma_ngoaite
			<< " | Mua tiền mặt: " << tienmat
			<< " | Mua chuyển khoản: " << chuyenkhoan
			<< " | Bán: " << giaban << " ... ";

		if (capnhat_tygia(tygia.ma_ngoaite, tienmat, chuyenkhoan, giaban))
			cout << "Cập nhật thành công!" << endl;
		else
			cerr << "Lỗi cập nhật!" << endl;
	}
}

int main()
{
	string html = lay_dl_tygia();
	if (html.empty())
	{
		cerr << "Lỗi: Không thể lấy dữ liệu từ Vietcombank!" << endl;
		return 1;
	}

	vector<ty_gia_ngoai_te> ds_tygia = loc_dl_html(html);
	if (ds_tygia.empty())
	{
		cerr << "Lỗi: Không tìm thấy dữ liệu tỷ giá trong HTML!" << endl;
	} else
	{
		capnhat_intb(ds_tygia);
		cout << "Quá trình cập nhật tỷ giá hoàn tất!" << endl;
	}
	return 0;
}
