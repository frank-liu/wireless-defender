#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/md5.h>
#include <time.h>

#include "wireless-defender.h"
#include "wdcp.h"
#include "server.h"
#include "utils.h"

static int WD_wdcp_check_login(const char *username, uint8_t username_len,
	const char *password);
static int WD_wdcp_process_data_req(struct packet *p);
static int WD_wdcp_req_basic_info(struct packet *p);
static int WD_wdcp_req_ap_list(struct packet *p);

static ssize_t WD_wdcp_recv(int sockfd, void *buf, size_t len, int flags);
static ssize_t WD_wdcp_send(int sockfd, void *buf, size_t len, int flags);
static void WD_wdcp_new_pkt(struct packet *p);
static void WD_wdcp_del_pkt(struct packet *p);
static void WD_wdcp_rst_pkt(struct packet *p);
static void WD_wdcp_send_pkt(struct packet *p);
static void WD_wdcp_recv_pkt(struct packet *p);
static void WD_wdcp_packet_write_n(struct packet *p, void *data, size_t len);
static void WD_wdcp_packet_write_u8(struct packet *p, uint8_t data);
static void WD_wdcp_packet_write_u32(struct packet *p, uint32_t data);
static void WD_wdcp_packet_write_u64(struct packet *p, uint64_t data);
static void WD_wdcp_packet_read_n(struct packet *p, void *data, size_t len);
static void WD_wdcp_packet_read_u8(struct packet *p, uint8_t *data);
static void WD_wdcp_packet_read_u32(struct packet *p, uint32_t *data);

int
WD_wdcp_build_connection()
{
	struct packet p;
	uint8_t type;
	uint32_t version, security_type;

	WD_wdcp_new_pkt(&p);
	// 接收请求连接数据包
	WD_wdcp_recv_pkt(&p);
	// 如果数据包长度大于规定长度则发送连接失败数据包
	if(p.len != WDCP_CONN_REQ_PKT_LEN) {
		goto fail;
	}
	// 如果数据包类型不为请求连接数据包则发送连接失败数据包
	WD_wdcp_packet_read_u8(&p, &type);
	if(type != CONN_REQ_PKT) {
		goto fail;
	}
	// 如果版本号不为0x00010000则发送连接失败数据包
	WD_wdcp_packet_read_u32(&p, &version);
	if(version != 0x00010000) {
		goto fail;
	}
	// 记录客户支持的加密方式
	WD_wdcp_packet_read_u32(&p, &security_type);
	
	// 发送连接响应数据包
	WD_wdcp_rst_pkt(&p);
	WD_wdcp_packet_write_u8(&p, CONN_RSP_PKT);
	WD_wdcp_packet_write_u32(&p, SEC_TYPE_STANDARD);
	WD_wdcp_send_pkt(&p);
	WD_wdcp_del_pkt(&p);
	return WDCP_CONNECTION_SUCCESS;

fail:
	// 发送连接失败数据包
	WD_wdcp_rst_pkt(&p);
	WD_wdcp_packet_write_u8(&p, CONN_FAIL_PKT);
	WD_wdcp_packet_write_u32(&p, FAILED_PROTOCOL_ERR);
	WD_wdcp_send_pkt(&p);
	WD_wdcp_del_pkt(&p);
	return WDCP_CONNECTION_FAIL;
}

int
WD_wdcp_authenticate()
{
	struct packet p;
	uint8_t type;
	uint8_t username_len;
	char username[256], password[32];
	uint32_t failure_code = FAILED_PROTOCOL_ERR;

	WD_wdcp_new_pkt(&p);
	// 接收认证请求数据包
	WD_wdcp_recv_pkt(&p);
	// 如果数据包类型不为请求连接数据包则发送连接失败数据包
	WD_wdcp_packet_read_u8(&p, &type);
	if(type != AUTH_REQ_PKT) {
		goto fail;
	}
	// 提取username_len与username
	WD_wdcp_packet_read_u8(&p, &username_len);
	WD_wdcp_packet_read_n(&p, username, username_len);
	if(username[username_len - 1] != '\0') {
		goto fail;
	}
	// 提取password
	WD_wdcp_packet_read_n(&p, password, sizeof(password));
	// 进行用户名密码认证
	if(AUTH_CHECK_FAIL ==
		WD_wdcp_check_login(username, username_len, password)) {
		failure_code = FAILED_AUTH_CHECK;
		goto fail;
	}
	
	// 发送认证成功数据包
	WD_wdcp_rst_pkt(&p);
	WD_wdcp_packet_write_u8(&p, AUTH_RSP_PKT);
	WD_wdcp_send_pkt(&p);
	WD_wdcp_del_pkt(&p);
	return WDCP_AUTHENTICATE_SUCCESS;

fail:
	// 发送认证失败数据包
	WD_wdcp_rst_pkt(&p);
	WD_wdcp_packet_write_u8(&p, AUTH_FAIL_PKT);
	WD_wdcp_packet_write_u32(&p, failure_code);
	WD_wdcp_send_pkt(&p);
	WD_wdcp_del_pkt(&p);
	return WDCP_AUTHENTICATE_FAIL;
}

int
WD_wdcp_process()
{
	struct packet p;
	uint8_t type;
	int result;

	WD_wdcp_new_pkt(&p);
	// 接收数据通信数据包
	WD_wdcp_recv_pkt(&p);
	// 取出数据包类型
	WD_wdcp_packet_read_u8(&p, &type);
	// 根据不同类型，进行不同处理
	switch(type) {
	case DATA_REQ_PKT:
		result = WD_wdcp_process_data_req(&p);
		break;
	default:
		result = WDCP_PROCESS_FAIL;
		break;
	}
	
	WD_wdcp_del_pkt(&p);

	return result;
}

static int
WD_wdcp_check_login(const char *username, uint8_t username_len,
	const char *password)
{
	char md5[32], *p;
	unsigned char md[MD5_DIGEST_LENGTH];
	int i;

	// 判断用户名是否正确
	if(0 != strncmp(username, AUTH_DEFAULT_USERNAME, username_len)) {
		goto fail;
	}

	// 判断密码是否正确
	MD5((const unsigned char *)AUTH_DEFAULT_PASSWORD,
		strlen(AUTH_DEFAULT_PASSWORD), md);
	p = md5;
	for(i = 0; i < MD5_DIGEST_LENGTH; i++) {
		p += sprintf(p, "%2.2x", md[i]);
	}
	if(0 != strncmp(password, md5, sizeof(md5))) {
		goto fail;
	}

	return AUTH_CHECK_SUCCESS;

fail:
	return AUTH_CHECK_FAIL;
}

static int
WD_wdcp_process_data_req(struct packet *p)
{
	uint8_t req_type;

	// 取request_type
	WD_wdcp_packet_read_u8(p, &req_type);
	switch(req_type) {
	case REQ_TYPE_BASIC_INFO:
		WD_wdcp_req_basic_info(p);
		break;
	case REQ_TYPE_AP_LIST:
		WD_wdcp_req_ap_list(p);
		break;
	default:
		break;
	}

	return WDCP_PROCESS_SUCCESS;
}

static int
WD_wdcp_req_basic_info(struct packet *p)
{
	WD_wdcp_rst_pkt(p);

	// 写入运行时间
	WD_wdcp_packet_write_u64(p, time(NULL) - WD_start_time);

	// 发送数据包
	WD_wdcp_send_pkt(p);

	return WDCP_PROCESS_SUCCESS;
}

static int
WD_wdcp_req_ap_list(struct packet *p)
{
	WD_wdcp_rst_pkt(p);

	// 写入AP个数
	WD_wdcp_packet_write_u8(p, 0);

	// 发送数据包
	WD_wdcp_send_pkt(p);

	return WDCP_PROCESS_SUCCESS;
}

static ssize_t
WD_wdcp_recv(int sockfd, void *buf, size_t len, int flags)
{
	ssize_t n;

	n = recv(client_fd, buf, len, 0);
	if(n == -1) {
		err_exit("receive data from client error");
	}

	return n;
}

static ssize_t
WD_wdcp_send(int sockfd, void *buf, size_t len, int flags)
{
	ssize_t n;

	n = send(client_fd, buf, sizeof(buf), 0);
	if(n == -1) {
		err_exit("send data to client error");
	}

	return n;
}

static void
WD_wdcp_new_pkt(struct packet *p)
{
	p->buf = malloc(WDCP_PACKET_LEN);
	if(p->buf == NULL) {
		err_exit("create new packet error");
	}
	p->p = p->buf;
}

static void
WD_wdcp_del_pkt(struct packet *p)
{
	free(p->buf);
}

static void
WD_wdcp_rst_pkt(struct packet *p)
{
	p->p = p->buf;
	p->len = 0;
}

static void
WD_wdcp_send_pkt(struct packet *p)
{
	WD_wdcp_send(client_fd, p->buf, p->p - p->buf, 0);
}

static void
WD_wdcp_recv_pkt(struct packet *p)
{
	p->len = WD_wdcp_recv(client_fd, p->buf, WDCP_PACKET_LEN, 0);
	p->p = p->buf;
}

static void
WD_wdcp_packet_write_n(struct packet *p, void *data, size_t len)
{
	memcpy(p->p, data, len);
	p->p += len;
}

static void
WD_wdcp_packet_write_u8(struct packet *p, uint8_t data)
{
	WD_wdcp_packet_write_n(p, &data, sizeof(data));
}

static void
WD_wdcp_packet_write_u32(struct packet *p, uint32_t data)
{
	WD_wdcp_packet_write_n(p, &data, sizeof(data));
}

static void
WD_wdcp_packet_write_u64(struct packet *p, uint64_t data)
{
	WD_wdcp_packet_write_n(p, &data, sizeof(data));
}

static void
WD_wdcp_packet_read_n(struct packet *p, void *data, size_t len)
{
	memcpy(data, p->p, len);
	p->p += len;
}

static void
WD_wdcp_packet_read_u8(struct packet *p, uint8_t *data)
{
	WD_wdcp_packet_read_n(p, data, sizeof(*data));
}

static void
WD_wdcp_packet_read_u32(struct packet *p, uint32_t *data)
{
	WD_wdcp_packet_read_n(p, data, sizeof(*data));
}
