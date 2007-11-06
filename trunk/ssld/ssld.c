#include "stdinc.h"

#include <zlib.h>

#define MAXPASSFD 4
#ifndef READBUF_SIZE
#define READBUF_SIZE 16384
#endif

static void conn_mod_write_sendq(rb_fde_t *, void *data);
static void conn_plain_write_sendq(rb_fde_t *, void *data);

static char inbuf[READBUF_SIZE];
static char outbuf[READBUF_SIZE];


typedef struct _mod_ctl_buf
{
        rb_dlink_node node;
        char buf[READBUF_SIZE];
        size_t buflen;
        rb_fde_t *F[MAXPASSFD];
        int nfds;
} mod_ctl_buf_t; 


typedef struct _mod_ctl
{
        rb_dlink_node node;
        int cli_count;
        rb_fde_t *F;  
        rb_fde_t *F_pipe;
        rb_dlink_list readq;
        rb_dlink_list writeq;
} mod_ctl_t;

typedef struct _conn
{
        rb_dlink_node node;
        rawbuf_head_t *modbuf_out;
        rawbuf_head_t *plainbuf_out;

        rb_fde_t *mod_fd;
        rb_fde_t *plain_fd;
        unsigned long mod_out;
        unsigned long mod_in;
        unsigned long plain_in;
        unsigned long plain_out;
	rb_uint8_t is_ssl;
#ifdef HAVE_ZLIB
	rb_uint8_t is_zlib;
	z_stream	instream;
	z_stream outstream;
#endif
} conn_t;

static void
close_conn(conn_t * conn)
{
        rb_rawbuf_flush(conn->modbuf_out, conn->mod_fd);
        rb_rawbuf_flush(conn->plainbuf_out, conn->plain_fd);
        rb_close(conn->mod_fd); 
        rb_close(conn->plain_fd);

        rb_free_rawbuffer(conn->modbuf_out);
        rb_free_rawbuffer(conn->plainbuf_out);
        memset(conn, 0, sizeof(conn_t));
        rb_free(conn);
}

static conn_t *
make_conn(rb_fde_t *mod_fd, rb_fde_t *plain_fd)
{
        conn_t *conn = rb_malloc(sizeof(conn_t));
        conn->modbuf_out = rb_new_rawbuffer();
        conn->plainbuf_out = rb_new_rawbuffer();
        conn->mod_fd = mod_fd;
        conn->plain_fd = plain_fd;
        return conn;
}




static void
conn_mod_write_sendq(rb_fde_t *fd, void *data)
{
        conn_t *conn = data;
        int retlen;
        while ((retlen = rb_rawbuf_flush(conn->modbuf_out, fd)) > 0)
        {
                conn->mod_out += retlen;
        }
        if(retlen == 0 || (retlen < 0 && !rb_ignore_errno(errno)))
        {
                close_conn(data);
                return;
        }
        if(rb_rawbuf_length(conn->modbuf_out) > 0) {
                rb_setselect(conn->mod_fd, RB_SELECT_WRITE, conn_mod_write_sendq, conn);
        }
        else {
                rb_setselect(conn->mod_fd, RB_SELECT_WRITE, NULL, NULL);
        }
}
 
static void
conn_mod_write(conn_t * conn, void *data, size_t len)
{
        rb_rawbuf_append(conn->modbuf_out, data, len);
}
 
static void
conn_plain_write(conn_t * conn, void *data, size_t len)
{
        rb_rawbuf_append(conn->plainbuf_out, data, len);
} 

#ifdef HAVE_ZLIB
static void
common_zlib_deflate(conn_t *conn, void *buf, size_t len)
{
	int ret, have;
	conn->mod_in += len;
	conn->outstream.next_in = buf;
	conn->outstream.avail_in = len;
	conn->outstream.next_out = (Bytef *)outbuf;
	conn->outstream.avail_out = sizeof(outbuf);
	
	ret = deflate(&conn->outstream, Z_SYNC_FLUSH);	
	if(ret != Z_OK)
	{
		/* XXX deflate error */
	}
	if(conn->outstream.avail_out == 0)
	{
		/* XXX deal with avail_out being empty */
	}
	if(conn->outstream.avail_in != 0)
	{
		/* XXX deal with avail_in not being empty */
	}
	have = sizeof(outbuf) - conn->outstream.avail_out;
	conn_mod_write(conn, outbuf, have);
}

static void
common_zlib_inflate(conn_t *conn, void *buf, size_t len)
{
	int ret, have;
	conn->mod_in += len;
	conn->instream.next_in = buf;
	conn->instream.avail_in = len;
	conn->instream.next_out = (Bytef *)outbuf;
	conn->instream.avail_out = sizeof(outbuf);

	while(conn->instream.avail_in)
	{
		ret = inflate(&conn->instream, Z_NO_FLUSH);
		if(ret != Z_OK)
		{
			if(!strncmp("ERROR ", buf, 6))
			{
				/* XXX deal with error */
			}
			/* other error */
			close_conn(conn);
			return;			
		}
		have = sizeof(outbuf) - conn->instream.avail_out;
		
		if(conn->instream.avail_in)
		{
			conn_plain_write(conn, outbuf, have);
			have = 0;
			conn->instream.next_out = (Bytef *)outbuf;
			conn->instream.avail_out = sizeof(outbuf);
		}
	}
	if(have == 0)
		return;

	conn_plain_write(conn, outbuf, have);
}
#endif

static void
conn_plain_read_cb(rb_fde_t *fd, void *data)
{
        conn_t *conn = data;
        int length = 0;
        if(conn == NULL)
                return; 

        while ((length = rb_read(conn->plain_fd, inbuf, sizeof(inbuf))) > 0)
        {
                conn->plain_in += length;
#ifdef HAVE_ZLIB
                if(conn->is_zlib)
                	common_zlib_deflate(conn, inbuf, length);
		else
#endif
	                conn_mod_write(conn, inbuf, length);
        }
        if(length == 0 || (length < 0 && !rb_ignore_errno(errno)))
        {
                close_conn(conn);
                return;
        }

        rb_setselect(conn->plain_fd, RB_SELECT_READ, conn_plain_read_cb, conn);
        conn_mod_write_sendq(conn->mod_fd, conn);

}

static void
conn_mod_read_cb(rb_fde_t *fd, void *data)
{
        conn_t *conn = data;
        int length;
        if(conn == NULL)
                return; 
        while ((length = rb_read(conn->mod_fd, inbuf, sizeof(inbuf))) > 0)
        {
        	conn->mod_in += length;
#ifdef HAVE_ZLIB
        	if(conn->is_zlib)
	        	common_zlib_inflate(conn, inbuf, length);
		else
#endif
	                conn_plain_write(conn, inbuf, length);
        }
        if(length == 0 || (length < 0 && !rb_ignore_errno(errno)))
        {
                close_conn(conn);
                return;
        }
        rb_setselect(conn->mod_fd, RB_SELECT_READ, conn_mod_read_cb, conn);
        conn_plain_write_sendq(conn->plain_fd, conn);
}

static void
conn_plain_write_sendq(rb_fde_t *fd, void *data)
{
        conn_t *conn = data;
        int retlen;
        while ((retlen = rb_rawbuf_flush(conn->plainbuf_out, fd)) > 0)
        {
                conn->plain_out += retlen;
        }
        if(retlen == 0 || (retlen < 0 && !rb_ignore_errno(errno)))
        {  
                close_conn(data);
                return;
        }

        if(rb_rawbuf_length(conn->plainbuf_out) > 0)
                rb_setselect(conn->plain_fd, RB_SELECT_WRITE, conn_plain_write_sendq, conn);
        else
                rb_setselect(conn->plain_fd, RB_SELECT_WRITE, NULL, NULL);
}


static void
mod_main_loop(void)
{
	while(1)
	{
		rb_select(1000);
		rb_event_run();	
	}


}


static int
maxconn(void)
{
#if defined(RLIMIT_NOFILE) && defined(HAVE_SYS_RESOURCE_H)
        struct rlimit limit;

        if(!getrlimit(RLIMIT_NOFILE, &limit))
        {
                return limit.rlim_cur;
        }
#endif /* RLIMIT_FD_MAX */
        return MAXCONNECTIONS;
}

static void
ssl_process_accept_cb(rb_fde_t *F, int status, struct sockaddr *addr, rb_socklen_t len, void *data)
{
	conn_t *conn = data;
	if(status == RB_OK)
	{
		conn_mod_read_cb(conn->mod_fd, conn);
		conn_plain_read_cb(conn->plain_fd, conn);
		return;
	}
	close_conn(conn);
	return;
}

static void
ssl_process_accept(mod_ctl_buf_t *ctlb)
{
	conn_t *conn;
	conn = make_conn(ctlb->F[0], ctlb->F[1]);
	conn->is_ssl = 1;
	rb_ssl_start_accepted(ctlb->F[0], ssl_process_accept_cb, conn);
	return;
}

static void
ssl_process_connect(mod_ctl_buf_t *ctlb)
{
	/* XXX write me */
	return;
}
#ifdef HAVE_ZLIB
static void
zlib_process(mod_ctl_buf_t *ctlb)
{
	conn_t *conn;
	void *leftover;
	conn = make_conn(ctlb->F[0], ctlb->F[1]);
	conn->is_ssl = 0;
	conn->is_zlib = 1;
	
	conn->instream.total_in = 0;
	conn->instream.total_out = 0;
	conn->instream.zalloc = (alloc_func) 0;
	conn->instream.zfree = (free_func) 0;
	conn->instream.data_type = Z_ASCII;
	inflateInit(&conn->instream);

	conn->outstream.total_in = 0;
	conn->outstream.total_out = 0;
	conn->outstream.zalloc = (alloc_func) 0;
	conn->outstream.zfree = (free_func) 0;
	conn->outstream.data_type = Z_ASCII;
	
	deflateInit(&conn->outstream, Z_DEFAULT_COMPRESSION);
	
	if(ctlb->buflen > 1)
	{	
		leftover = ctlb->buf + sizeof(char);
		common_zlib_inflate(conn, leftover, ctlb->buflen - sizeof(char));
	}
        conn_mod_read_cb(conn->mod_fd, conn);
	conn_plain_read_cb(conn->plain_fd, conn);
	return;
}
#endif



static void
mod_process_cmd_recv(mod_ctl_t *ctl)
{
        rb_dlink_node *ptr, *next;      
        mod_ctl_buf_t *ctl_buf;                 

        RB_DLINK_FOREACH_SAFE(ptr, next, ctl->readq.head)
        {
                ctl_buf = ptr->data;

                switch(*ctl_buf->buf)
                {
                        case 'A':
                        {
				ssl_process_accept(ctl_buf);
                                break;
                        }
			case 'C':
			{
				ssl_process_connect(ctl_buf);
				break;
			}
#ifdef HAVE_ZLIB
			case 'Z':
			{
				/* just zlib only */
				zlib_process(ctl_buf);
				break;
			}	
#endif
                        default:
                                break;   
                                /* Log unknown commands */
                }                               
                rb_dlinkDelete(ptr, &ctl->readq);
//                rb_free(ctl_buf->buf);
                rb_free(ctl_buf);
        }

}



static void
mod_read_ctl(rb_fde_t *F, void *data)  
{
        mod_ctl_buf_t *ctl_buf;
        mod_ctl_t *ctl = data;
        int retlen;

        do
        {
                ctl_buf = rb_malloc(sizeof(mod_ctl_buf_t));

                retlen = rb_recv_fd_buf(ctl->F, ctl_buf->buf, sizeof(ctl_buf->buf), ctl_buf->F, MAXPASSFD);
                if(retlen <= 0) 
                        rb_free(ctl_buf);
                else {
                	ctl_buf->buflen = retlen;
                        rb_dlinkAddTail(ctl_buf, &ctl_buf->node, &ctl->readq);
		}
        } while(retlen > 0);    

        if(retlen == 0 || (retlen < 0 && !rb_ignore_errno(errno)))
        {
		exit(0);        	
        }
        mod_process_cmd_recv(ctl);
        rb_setselect(ctl->F, RB_SELECT_READ, mod_read_ctl, ctl);
}

static void
mod_write_ctl(rb_fde_t *F, void *data)
{
        mod_ctl_t *ctl = data;
        mod_ctl_buf_t *ctl_buf;
        rb_dlink_node *ptr, *next;
        int retlen, x;

        RB_DLINK_FOREACH_SAFE(ptr, next, ctl->writeq.head)
        {
                ctl_buf = ptr->data;

                retlen = rb_send_fd_buf(ctl->F, ctl_buf->F, ctl_buf->nfds,  ctl_buf->buf, ctl_buf->buflen);
                if(retlen > 0)
                {
                        rb_dlinkDelete(ptr, &ctl->writeq);
                        for(x = 0; x < ctl_buf->nfds; x++)
                                rb_close(ctl_buf->F[x]);  
                        rb_free(ctl_buf->buf);
                        rb_free(ctl_buf);
                 
                }
                if(retlen == 0 || (retlen < 0 && !rb_ignore_errno(errno)))
                {
                        /* deal with failure here */
                } else  {
                        rb_setselect(ctl->F, RB_SELECT_WRITE, mod_write_ctl, ctl);
                }
        }
}
#if 0
static void
mod_cmd_write_queue(mod_ctl_t *ctl, rb_fde_t **F, int count, const char *buf)
{
        mod_ctl_buf_t *ctl_buf;
        int x;  
        ctl_buf = rb_malloc(sizeof(mod_ctl_buf_t));
        rb_strlcpy(ctl_buf->buf, buf, sizeof(ctl_buf->buf));

        for(x = 0; x < count && x < MAXPASSFD; x++)
        {
                ctl_buf->F[x] = F[x];
        }
        rb_dlinkAddTail(ctl_buf, &ctl_buf->node, &ctl->readq);

}
#endif

static void
read_pipe_ctl(rb_fde_t *F, void *data)
{
	int retlen;
	while((retlen = rb_read(F, inbuf, sizeof(inbuf))) > 0)
	{
		;; /* we don't do anything with the pipe really, just care if the other process dies.. */
	}
	if(retlen == 0 || (retlen < 0 && !rb_ignore_errno(errno)))
		exit(0);
	rb_setselect(F, RB_SELECT_READ, read_pipe_ctl, NULL); 
	
}

int main(int argc, char **argv)
{
	mod_ctl_t *ctl;
	const char *s_ctlfd, *s_pipe;
	const char *ssl_cert, *ssl_private_key, *ssl_dh_params;
	int ctlfd, pipefd, x, maxfd;

	maxfd = maxconn();
	s_ctlfd = getenv("CTL_FD");
	s_pipe = getenv("CTL_PIPE");

	if(s_ctlfd == NULL || s_pipe == NULL)
	{
		fprintf(stderr, "You aren't supposed to run me directly\n");
		exit(1);
	 	/* xxx fail */
	}
	
	ctlfd = atoi(s_ctlfd);
	pipefd = atoi(s_pipe);
	ssl_cert = getenv("SSL_CERT");
	ssl_private_key = getenv("SSL_PRIVATE_KEY");
	ssl_dh_params = getenv("SSL_DH_PARAMS");
		
	for(x = 0; x < maxfd; x++)
	{
		if(x != ctlfd && x != pipefd && x > 2)
			close(x);
	}
	rb_lib_init(NULL, NULL, NULL, 0, maxfd, 1024, 1024, 4096);
	rb_init_rawbuffers(1024);
	rb_setup_ssl_server(ssl_cert, ssl_private_key, ssl_dh_params);
	ctl = rb_malloc(sizeof(mod_ctl_t));
	ctl->F = rb_open(ctlfd, RB_FD_SOCKET, "ircd control socket");
	ctl->F_pipe = rb_open(pipefd, RB_FD_PIPE, "ircd pipe");	
	read_pipe_ctl(ctl->F_pipe, NULL);
	mod_read_ctl(ctl->F, ctl);
	mod_main_loop();	
	return 0;
} 

