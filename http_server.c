#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>

#include "bignum.h"
#include "http_parser.h"
#include "http_server.h"

#define CRLF "\r\n"

#define HTTP_RESPONSE_200_DUMMY                               \
    ""                                                        \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF     \
    "Content-Type: text/plain" CRLF "Content-Length: %d" CRLF \
    "Connection: Close" CRLF CRLF "%s" CRLF
#define HTTP_RESPONSE_200_KEEPALIVE_DUMMY                     \
    ""                                                        \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF     \
    "Content-Type: text/plain" CRLF "Content-Length: %d" CRLF \
    "Connection: Keep-Alive" CRLF CRLF "%s" CRLF
#define HTTP_RESPONSE_501                                              \
    ""                                                                 \
    "HTTP/1.1 501 Not Implemented" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/plain" CRLF "Content-Length: 21" CRLF          \
    "Connection: Close" CRLF CRLF "501 Not Implemented" CRLF
#define HTTP_RESPONSE_501_KEEPALIVE                                    \
    ""                                                                 \
    "HTTP/1.1 501 Not Implemented" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/plain" CRLF "Content-Length: 21" CRLF          \
    "Connection: KeepAlive" CRLF CRLF "501 Not Implemented" CRLF

#define RECV_BUFFER_SIZE 4096

struct http_request {
    struct socket *socket;
    enum http_method method;
    char request_url[128];
    int complete;
};

struct khttpd_service daemon = {.is_stopped = false};
extern struct workqueue_struct *khttpd_wq;

static int http_server_recv(struct socket *sock, char *buf, size_t size)
{
    struct kvec iov = {.iov_base = (void *) buf, .iov_len = size};
    struct msghdr msg = {.msg_name = 0,
                         .msg_namelen = 0,
                         .msg_control = NULL,
                         .msg_controllen = 0,
                         .msg_flags = 0};
    return kernel_recvmsg(sock, &msg, &iov, 1, size, msg.msg_flags);
}

static int http_server_send(struct socket *sock, const char *buf, size_t size)
{
    struct msghdr msg = {.msg_name = NULL,
                         .msg_namelen = 0,
                         .msg_control = NULL,
                         .msg_controllen = 0,
                         .msg_flags = 0};
    int done = 0;
    while (done < size) {
        struct kvec iov = {
            .iov_base = (void *) ((char *) buf + done),
            .iov_len = size - done,
        };
        int length = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
        if (length < 0) {
            pr_err("write error: %d\n", length);
            break;
        }
        done += length;
    }
    return done;
}

static int _log10(size_t N)
{
    int i;
    unsigned int vals[] = {
        1UL,      10UL,      100UL,      1000UL,      10000UL,
        100000UL, 1000000UL, 10000000UL, 100000000UL, 1000000000UL,
    };
    for (i = 0; i < 9; ++i) {
        if (N >= vals[i] && N < vals[i + 1]) {
            break;
        }
    }
    return i + 1;
}

static char *fib(unsigned long long k)
{
    char *newstr, *p;
    int index = 0;
    bignum a, b;
    int i = 31 - __builtin_clz(k);
    bignum big_two;
    int_to_bignum(0, &a);
    int_to_bignum(1, &b);
    int_to_bignum(2, &big_two);
    for (; i >= 0; i--) {
        bignum t1, t2;
        bignum tmp1, tmp2;
        multiply_bignum(&b, &big_two, &tmp1);
        (void) subtract_bignum(&tmp1, &a, &tmp2);
        multiply_bignum(&a, &tmp2, &t1);
        multiply_bignum(&a, &a, &tmp1);
        multiply_bignum(&b, &b, &tmp2);
        (void) add_bignum(&tmp1, &tmp2, &t2);
        copy(&a, &t1);
        copy(&b, &t2);
        if ((k & (1 << i)) > 0) {
            (void) add_bignum(&a, &b, &t1);
            copy(&a, &b);
            copy(&b, &t1);
        }
    }
    newstr = kmalloc(a.lastdigit + 1, GFP_KERNEL);
    if (!newstr)
        return NULL;
    p = newstr;
    /* copy the string. */
    while (index < a.lastdigit) {
        *p++ = a.digits[index];
        index++;
    }
    *p = '\0';
    return newstr;
}

static char *parse_url(char *url, int keep_alive)
{
    size_t size;
    char const *del = "/";
    char *token, *tmp = url, *fib_res = NULL, *res;
    char *msg = keep_alive ? HTTP_RESPONSE_200_KEEPALIVE_DUMMY
                           : HTTP_RESPONSE_200_DUMMY;
    tmp++;
    token = strsep(&tmp, del);
    if (strcmp(token, "fib") == 0) {
        /* expected tmp is number now */
        long k;
        kstrtol(tmp, 10, &k);
        fib_res = fib((unsigned long long) k);
    }
    if (!fib_res)
        return NULL;
    size = strlen(msg) + strlen(fib_res) + _log10(strlen(fib_res)) - 4;
    res = kmalloc(size, GFP_KERNEL);
    snprintf(res, size, msg, strlen(fib_res), fib_res);

    return res;
}

static int http_server_response(struct http_request *request, int keep_alive)
{
    char *response;

    pr_info("requested_url = %s\n", request->request_url);
    if (request->method != HTTP_GET)
        response = keep_alive ? HTTP_RESPONSE_501_KEEPALIVE : HTTP_RESPONSE_501;
    else
        response = parse_url(request->request_url, keep_alive);

    if (response)
        http_server_send(request->socket, response, strlen(response));

    return 0;
}

static int http_parser_callback_message_begin(http_parser *parser)
{
    struct http_request *request = parser->data;
    struct socket *socket = request->socket;
    memset(request, 0x00, sizeof(struct http_request));
    request->socket = socket;
    return 0;
}

static int http_parser_callback_request_url(http_parser *parser,
                                            const char *p,
                                            size_t len)
{
    struct http_request *request = parser->data;
    strncat(request->request_url, p, len);
    return 0;
}

static int http_parser_callback_header_field(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_header_value(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_headers_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    request->method = parser->method;
    return 0;
}

static int http_parser_callback_body(http_parser *parser,
                                     const char *p,
                                     size_t len)
{
    return 0;
}

static int http_parser_callback_message_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    http_server_response(request, http_should_keep_alive(parser));
    request->complete = 1;
    return 0;
}

static void http_server_worker(struct work_struct *work)
{
    char *buf;
    struct http_parser parser;
    struct http_parser_settings setting = {
        .on_message_begin = http_parser_callback_message_begin,
        .on_url = http_parser_callback_request_url,
        .on_header_field = http_parser_callback_header_field,
        .on_header_value = http_parser_callback_header_value,
        .on_headers_complete = http_parser_callback_headers_complete,
        .on_body = http_parser_callback_body,
        .on_message_complete = http_parser_callback_message_complete};
    struct http_request request;
    struct khttpd_work_data *worker =
        container_of(work, struct khttpd_work_data, khttpd_work);

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    buf = kmalloc(RECV_BUFFER_SIZE, GFP_KERNEL);
    if (!buf) {
        pr_err("can't allocate memory!\n");
        return;
    }

    request.socket = worker->sock;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = &request;
    while (!daemon.is_stopped) {
        int ret = http_server_recv(worker->sock, buf, RECV_BUFFER_SIZE - 1);
        if (ret <= 0) {
            if (ret)
                pr_err("recv error: %d\n", ret);
            break;
        }
        http_parser_execute(&parser, &setting, buf, ret);
        if (request.complete && !http_should_keep_alive(&parser))
            break;
    }
    kernel_sock_shutdown(worker->sock, SHUT_RDWR);
    sock_release(worker->sock);
    kfree(worker);
    kfree(buf);
    return 0;
}

struct work_struct *create_work(struct socket *socket)
{
    struct khttpd_work_data *wdata;
    if (!(wdata = kmalloc(sizeof(struct khttpd_work_data), GFP_KERNEL)))
        return NULL;

    wdata->sock = socket;
    INIT_WORK(&wdata->khttpd_work, http_server_worker);
    return &wdata->khttpd_work;
}

int http_server_daemon(void *arg)
{
    struct socket *socket;
    struct work_struct *work;
    struct http_server_param *param = (struct http_server_param *) arg;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    while (!kthread_should_stop()) {
        int err = kernel_accept(param->listen_socket, &socket, 0);
        if (err < 0) {
            if (signal_pending(current))
                break;
            pr_err("kernel_accept() error: %d\n", err);
            continue;
        }
        if (unlikely(!(work = create_work(socket)))) {
            pr_err("can't create more worker process\n");
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            continue;
        }
        queue_work(khttpd_wq, work);
    }
    daemon.is_stopped = true;
    return 0;
}
