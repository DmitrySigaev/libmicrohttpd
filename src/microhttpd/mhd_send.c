/*
  This file is part of libmicrohttpd
  Copyright (C) 2019 ng0 <ng0@n0.is>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

 */

/**
 * @file microhttpd/mhd_send.c
 * @brief Implementation of send() wrappers.
 * @author ng0 <ng0@n0.is>
 */

/* TODO: sendfile() wrappers. */

/* Functions to be used in: send_param_adapter, MHD_send_
 * and every place where sendfile(), sendfile64(), setsockopt()
 * are used. */

#include "mhd_send.h"

/**
 * Set socket to nodelay, on or off.
 *
 * @param connection the MHD_Connection structure
 * @param value the state to set, boolean
 */
void
MHD_send_socket_state_nodelay_ (struct MHD_Connection *connection,
                                bool value)
{
#if TCP_NODELAY
  const MHD_SCKT_OPT_BOOL_ state_val = value ? 1 : 0;

  if (0 == setsockopt (connection->socket_fd,
                       IPPROTO_TCP,
                       TCP_NODELAY,
                       (const void *) &state_val,
                       sizeof (state_val)))
    {
      connection->sk_tcp_nodelay_on = value;
    }
#endif
}

void
MHD_send_socket_state_nopush_ (struct MHD_Connection *connection,
                               bool value,
                               bool state_store)
{
#if TCP_NOPUSH
  const MHD_SCKT_OPT_BOOL_ state_val = value ? 1 : 0;

  if (0 == setsockopt (connection->socket_fd,
                       IPPROTO_TCP,
                       TCP_NOPUSH,
                       (const void *) &state_val,
                       sizeof (state_val)))
    {
      /* When TRUE above, this is usually FALSE, but
       * not always. We can't use the negation of
       * value for that reason. */
      connection->sk_tcp_nodelay_on = state_store;
    }
#endif
}

/**
 * Send buffer on connection, and remember the current state of
 * the socket options; only call setsockopt when absolutely
 * necessary.
 *
 * @param connection the MHD_Connection structure
 * @param buffer content of the buffer to send
 * @param buffer_size the size of the buffer (in bytes)
 * @param options the #MHD_SendSocketOptions enum,
 *         #MHD_SSO_NO_CORK: definitely no corking (use NODELAY, or explicitly disable cork),
 *         #MHD_SSO_MAY_CORK: should enable corking (use MSG_MORE, or explicitly enable cork),
 *         #MHD_SSO_HDR_CORK: consider tcpi_snd_mss and consider not corking for the header
 *         part if the size of the header is close to the MSS.
 *         Only used if we are NOT doing 100 Continue and are still sending the
 *         header (provided in full as the buffer to #MHD_send_on_connection_ or as
 *         the header to #MHD_send_on_connection2_).
 * @return sum of the number of bytes sent from both buffers or
 *         -1 on error
 */
ssize_t
MHD_send_on_connection_ (struct MHD_Connection *connection,
                         const char *buffer,
                         size_t buffer_size,
                         enum MHD_SendSocketOptions options)
{
  bool want_cork;
  bool have_cork;
  bool have_more;
  bool use_corknopush;
  bool using_tls = false;
  MHD_socket s = connection->socket_fd;
  ssize_t ret;
  const MHD_SCKT_OPT_BOOL_ off_val = 0;
  const MHD_SCKT_OPT_BOOL_ on_val = 1;

  /* error handling from send_param_adapter() */
  if ((MHD_INVALID_SOCKET == s) || (MHD_CONNECTION_CLOSED == connection->state))
  {
    return MHD_ERR_NOTCONN_;
  }

  /* from send_param_adapter() */
  if (buffer_size > MHD_SCKT_SEND_MAX_SIZE_)
    buffer_size = MHD_SCKT_SEND_MAX_SIZE_; /* return value limit */

  /* Get socket options, change/set options if necessary. */
  switch (options)
  {
  /* No corking */
  case MHD_SSO_NO_CORK:
    want_cork = false;
    break;
  /* Do corking, consider MSG_MORE instead if available. */
  case MHD_SSO_MAY_CORK:
    want_cork = true;
    break;
  /* Cork the header. */
  case MHD_SSO_HDR_CORK:
    want_cork = (buffer_size >= 1024) && (buffer_size <= 1220);
    break;
  }

  /* ! could be avoided by redefining the variable. */
  have_cork = ! connection->sk_tcp_nodelay_on;

#ifdef MSG_MORE
  have_more = true;
#else
  have_more = false;
#endif

#if TCP_NODELAY
  use_corknopush = false;
#elif TCP_CORK
  use_corknopush = true;
#elif TCP_NOPUSH
  use_corknopush = true;
#endif

#ifdef HTTPS_SUPPORT
  using_tls = (0 != (connection->daemon->options & MHD_USE_TLS));
#endif

#if TCP_CORK
  /* When we have CORK, we can have NODELAY on the same system,
   * at least since Linux 2.2 and both can be combined since
   * Linux 2.5.71. For more details refer to tcp(7) on Linux.
   * No other system in 2019-06 has TCP_CORK. */
  if ((! using_tls) && (use_corknopush) && (have_cork && ! want_cork))
    {
      if (0 == setsockopt (s,
                           IPPROTO_TCP,
                           TCP_CORK,
                           (const void *) &off_val,
                           sizeof (off_val)))
      {
        connection->sk_tcp_nodelay_on = true;
      }
      else if (0 == setsockopt (s,
                                IPPROTO_TCP,
                                TCP_NODELAY,
                                (const void *) &on_val,
                                sizeof (on_val)))
      {
        connection->sk_tcp_nodelay_on = true;
      }
    }
#elif TCP_NOPUSH
  /* TCP_NOPUSH on FreeBSD is equal to cork on Linux, with the
   * exception that we know that TCP_NOPUSH will definitely
   * exist and we can disregard TCP_NODELAY unless requested. */
  if ((! using_tls) && (use_corknopush) && (have_cork && ! want_cork))
    {
      MHD_send_socket_state_nopush_ (connection, true, false);
      /*
      if (0 == setsockopt (s,
                           IPPROTO_TCP,
                           TCP_NOPUSH,
                           (const void *) &on_val,
                           sizeof (on_val)))
        {
          connection->sk_tcp_nodelay_on = false;
        }
      */
    }
#endif
#if TCP_NODELAY
  if ((! using_tls) && (! use_corknopush) && (! have_cork && want_cork))
    {
      /*
      if (0 == setsockopt (s,
                           IPPROTO_TCP,
                           TCP_NODELAY,
                           (const void *) &off_val,
                           sizeof (off_val)))
      {
        connection->sk_tcp_nodelay_on = false;
      }
      */
      MHD_send_socket_state_nodelay_ (connection, false);
    }
#endif

#ifdef HTTPS_SUPPORT

  if (using_tls)
  {
    if (want_cork && ! have_cork)
    {
      gnutls_record_cork (connection->tls_session);
      connection->sk_tcp_nodelay_on = false;
    }
    if (buffer_size > SSIZE_MAX)
      buffer_size = SSIZE_MAX;
    ret = gnutls_record_send (connection->tls_session,
                              buffer,
                              buffer_size);
    if ( (GNUTLS_E_AGAIN == ret) ||
         (GNUTLS_E_INTERRUPTED == ret) )
    {
#ifdef EPOLL_SUPPORT
      if (GNUTLS_E_AGAIN == ret)
        connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif
      return MHD_ERR_AGAIN_;
    }
    if (ret < 0)
    {
      /* Likely 'GNUTLS_E_INVALID_SESSION' (client communication
         disrupted); interpret as a hard error */
      return MHD_ERR_NOTCONN_;
    }
#ifdef EPOLL_SUPPORT
    /* Unlike non-TLS connections, do not reset "write-ready" if
     * sent amount smaller than provided amount, as TLS
     * connections may break data into smaller parts for sending. */
#endif /* EPOLL_SUPPORT */

    if (! want_cork && have_cork)
    {
      (void) gnutls_record_uncork (connection->tls_session, 0);
      connection->sk_tcp_nodelay_on = true;
    }
  }
  else
#endif
  {
    /* plaintext transmission */
#if MSG_MORE
    ret = send (s,
                buffer,
                buffer_size,
                MAYBE_MSG_NOSIGNAL | (want_cork ? MSG_MORE : 0));
#else
    ret = send (connection->socket_fd, buffer, buffer_size, MAYBE_MSG_NOSIGNAL);
#endif

    if (0 > ret)
    {
      const int err = MHD_socket_get_error_ ();

      if (MHD_SCKT_ERR_IS_EAGAIN_ (err))
      {
#if EPOLL_SUPPORT
        /* EAGAIN, no longer write-ready */
        connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
        return MHD_ERR_AGAIN_;
      }
      if (MHD_SCKT_ERR_IS_EINTR_ (err))
        return MHD_ERR_AGAIN_;
      if (MHD_SCKT_ERR_IS_ (err, MHD_SCKT_ECONNRESET_))
        return MHD_ERR_CONNRESET_;
      /* Treat any other error as hard error. */
      return MHD_ERR_NOTCONN_;
    }
#if EPOLL_SUPPORT
    else if (buffer_size > (size_t) ret)
      connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
  }
#if TCP_CORK
  if ((! using_tls) && (use_corknopush) && (! have_cork && want_cork && ! have_more))
    {
      if (0 == setsockopt (s,
                           IPPROTO_TCP,
                           TCP_CORK,
                           (const void *) &on_val,
                           sizeof (on_val)))
      {
        connection->sk_tcp_nodelay_on = false;
      }
      else if (0 == setsockopt (s,
                                IPPROTO_TCP,
                                TCP_NODELAY,
                                (const void *) &off_val,
                                sizeof (off_val)))
      {
        connection->sk_tcp_nodelay_on = false;
      }
    }
#elif TCP_NOPUSH
  /* We don't have MSG_MORE. The OS which implement NOPUSH implement
   * it in a similar way to TCP_CORK on Linux. This means we can just
   * disregard the else branch for TCP_NODELAY which we had to use
   * for the TCP_CORK case here. */
  if ((! using_tls) && (use_corknopush) && (have_cork && ! want_cork))
  {
    MHD_send_socket_state_nopush_ (connection, true, false);
    /*
    if (0 == setsockopt (s,
                         IPPROTO_TCP,
                         TCP_NOPUSH,
                         (const void*) &on_val,
                         sizeof (on_val)))
      {
        connection->sk_tcp_nodelay_on = false;
      }
    */
  }
#endif

#if TCP_NODELAY
  if ((! using_tls) && (! use_corknopush) && (have_cork && ! want_cork))
    {
      MHD_send_socket_state_nodelay_ (connection, true);
      /*
      if (0 == setsockopt (s,
                           IPPROTO_TCP,
                           TCP_NODELAY,
                           (const void *) &on_val,
                           sizeof (on_val)))
      {
        connection->sk_tcp_nodelay_on = true;
      }
      */
    }
#endif

  return ret;
}


/**
 * Send header followed by buffer on connection.
 * Uses writev if possible to send both at once
 * and returns the sum of the number of bytes sent from
 * both buffers, or -1 on error;
 * if writev is unavailable, this call MUST only send from 'header'
 * (as we cannot handle the case that the first write
 * succeeds and the 2nd fails!).
 *
 * @param connection the MHD_Connection structure
 * @param header content of header to send
 * @param header_size the size of the header (in bytes)
 * @param buffer content of the buffer to send
 * @param buffer_size the size of the buffer (in bytes)
 * @return sum of the number of bytes sent from both buffers or
 *         -1 on error
 */
ssize_t
MHD_send_on_connection2_ (struct MHD_Connection *connection,
                          const char *header,
                          size_t header_size,
                          const char *buffer,
                          size_t buffer_size)
{
#if defined(HAVE_SENDMSG) || defined(HAVE_WRITEV)
  MHD_socket s = connection->socket_fd;
  bool have_cork;
  bool have_more;
  int iovcnt;
  int eno;
  const MHD_SCKT_OPT_BOOL_ off_val = 0;
  struct iovec vector[2];

  have_cork = ! connection->sk_tcp_nodelay_on;
#if TCP_NODELAY
  use_corknopush = false;
#elif TCP_CORK
  use_corknopush = true;
#elif TCP_NOPUSH
  use_corknopush = true;
#endif

#if TCP_NODELAY
  if ((! use_corknopush) && (! have_cork && want_cork))
    {
      MHD_send_socket_state_nodelay_ (connection, false);
      /*
      if (0 == setsockopt (s,
                           IPPROTO_TCP,
                           TCP_NODELAY,
                           (const void *) &off_val,
                           sizeof (off_val)))
        {
          connection->sk_tcp_nodelay_on = false;
        }
      */
    }
#endif

  vector[0].iov_base = header;
  vector[0].iov_len = strlen (header);
  vector[1].iov_base = buffer;
  vector[1].iov_len = strlen (buffer);

#if HAVE_SENDMSG
  struct msghdr msg;
  msg.msg_iov = vector;
  memset(&msg, 0, sizeof(buffer + header));
  ret = sendmsg (s, vector, MAYBE_MSG_NOSIGNAL);
#elif HAVE_WRITEV
  iovcnt = sizeof (vector) / sizeof (struct iovec);
  ret = writev (s, vector, iovcnt);
#endif

#if TCP_CORK
  if (use_corknopush)
  {
    eno;

    eno = errno;
    if ((ret == header_len + buffer_len) && have_cork)
      {
        // Response complete, definitely uncork!
        if (0 == setsockopt (s,
                             IPPROTO_TCP,
                             TCP_CORK,
                             (const void *) &off_val,
                             sizeof (off_val)))
          {
            connection->sk_tcp_nodelay_on = true;
          }
      }
    errno = eno;
  }
  return ret;
#elif TCP_NOPUSH
  if (use_corknopush)
    {
      eno;

      eno = errno;
      if (ret == header_len + buffer_len)
        {
          /* Response complete, set NOPUSH to off */
          MHD_send_socket_state_nopush_ (connection, false, false);
          /*
          if (0 == setsockopt (s,
                               IPPROTO_TCP,
                               TCP_NOPUSH,
                               (const void *) &off_val,
                               sizeof (off_val)))
            {
              connection->sk_tcp_nodelay_on = false;
            }
          */
        }
      errno = eno;
    }
  return ret;
#endif

#else
  return MHD_send_on_connection_ (connection,
                                  header,
                                  header_size,
                                  MHD_SSO_HDR_CORK);
#endif
}

/* Worth considering for future improvements and additions:
 * NetBSD has no sendfile or sendfile64. The way to work
 * with this seems to be to mmap the file and write(2) as
 * large a chunk as possible to the socket. Alternatively,
 * use madvise(..., MADV_SEQUENTIAL). */

#if defined(_MHD_HAVE_SENDFILE)
/**
 * Function for sending responses backed by file FD.
 * A sendfile() wrapper which also performs cork/uncork
 * operations.
 *
 * @param connection the #MHD_Connection structure
 * @return actual number of bytes sent
 */
ssize_t
MHD_sendfile_on_connection_ (struct MHD_Connection *connection)
{
  // I'm looking for a version of sendfile_adapter() that *also* performs
  // cork/uncork operations. Specifically, we want to make sure that the
  // buffer is flushed after sendfile() is done (so setsockopt() behavior
  // equivalent to MHD_SSO_NO_CORK), and _also_ of course update the
  // setsockopt state, i.e. connection->sk_tcp_nodelay_on = true;

  bool want_cork = false;
  bool have_cork;
  bool have_more;
  bool use_corknopush;
  bool using_tls = false;
  ssize_t ret;
  const int file_fd = connection->response->fd;
  uint64_t left;
  uint64_t offsetu64;
#ifndef HAVE_SENDFILE64
  const uint64_t max_off_t = (uint64_t)OFF_T_MAX;
#else  /* HAVE_SENDFILE64 */
  const uint64_t max_off_t = (uint64_t)OFF64_T_MAX;
#endif /* HAVE_SENDFILE64 */
#ifdef MHD_LINUX_SOLARIS_SENDFILE
#ifndef HAVE_SENDFILE64
  off_t offset;
#else  /* HAVE_SENDFILE64 */
  off64_t offset;
#endif /* HAVE_SENDFILE64 */
#endif /* MHD_LINUX_SOLARIS_SENDFILE */
#ifdef HAVE_FREEBSD_SENDFILE
  off_t sent_bytes;
  int flags = 0;
#endif
#ifdef HAVE_DARWIN_SENDFILE
  off_t len;
#endif /* HAVE_DARWIN_SENDFILE */
  const bool used_thr_p_c = (0 != (connection->daemon->options & MHD_USE_THREAD_PER_CONNECTION));
  const size_t chunk_size = used_thr_p_c ? MHD_SENFILE_CHUNK_THR_P_C_ : MHD_SENFILE_CHUNK_;
  size_t send_size = 0;
  mhd_assert (MHD_resp_sender_sendfile == connection->resp_sender);

  offsetu64 = connection->response_write_position + connection->response->fd_off;
  left = connection->response->total_size - connection->response_write_position;
  /* Do not allow system to stick sending on single fast connection:
   * use 128KiB chunks (2MiB for thread-per-connection). */
  send_size = (left > chunk_size) ? chunk_size : (size_t) left;
  if (max_off_t < offsetu64)
    { /* Retry to send with standard 'send()'. */
      connection->resp_sender = MHD_resp_sender_std;
      return MHD_ERR_AGAIN_;
    }
#ifdef MHD_LINUX_SOLARIS_SENDFILE
#ifndef HAVE_SENDFILE64
  offset = (off_t) offsetu64;
  ret = sendfile (connection->socket_fd,
                  file_fd,
                  &offset,
                  send_size);
#else  /* HAVE_SENDFILE64 */
  offset = (off64_t) offsetu64;
  ret = sendfile64 (connection->socket_fd,
                    file_fd,
                    &offset,
                    send_size);
#endif /* HAVE_SENDFILE64 */
  if (0 > ret)
    {
      const int err = MHD_socket_get_error_();
      if (MHD_SCKT_ERR_IS_EAGAIN_(err))
        {
#ifdef EPOLL_SUPPORT
          /* EAGAIN --- no longer write-ready */
          connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
          return MHD_ERR_AGAIN_;
        }
      if (MHD_SCKT_ERR_IS_EINTR_ (err))
        return MHD_ERR_AGAIN_;
#ifdef HAVE_LINUX_SENDFILE
      if (MHD_SCKT_ERR_IS_(err,
                           MHD_SCKT_EBADF_))
        return MHD_ERR_BADF_;
      /* sendfile() failed with EINVAL if mmap()-like operations are not
         supported for FD or other 'unusual' errors occurred, so we should try
         to fall back to 'SEND'; see also this thread for info on
         odd libc/Linux behavior with sendfile:
         http://lists.gnu.org/archive/html/libmicrohttpd/2011-02/msg00015.html */
      connection->resp_sender = MHD_resp_sender_std;
      return MHD_ERR_AGAIN_;
#else  /* HAVE_SOLARIS_SENDFILE */
      if ( (EAFNOSUPPORT == err) ||
           (EINVAL == err) ||
           (EOPNOTSUPP == err) )
        { /* Retry with standard file reader. */
          connection->resp_sender = MHD_resp_sender_std;
          return MHD_ERR_AGAIN_;
        }
      if ( (ENOTCONN == err) ||
           (EPIPE == err) )
        {
          return MHD_ERR_CONNRESET_;
        }
      return MHD_ERR_BADF_; /* Fail hard */
#endif /* HAVE_SOLARIS_SENDFILE */
    }
#ifdef EPOLL_SUPPORT
  else if (send_size > (size_t)ret)
        connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
#elif defined(HAVE_FREEBSD_SENDFILE)
#ifdef SF_FLAGS
  flags = used_thr_p_c ?
      freebsd_sendfile_flags_thd_p_c_ : freebsd_sendfile_flags_;
#endif /* SF_FLAGS */
  if (0 != sendfile (file_fd,
                     connection->socket_fd,
                     (off_t) offsetu64,
                     send_size,
                     NULL,
                     &sent_bytes,
                     flags))
    {
      const int err = MHD_socket_get_error_();
      if (MHD_SCKT_ERR_IS_EAGAIN_(err) ||
          MHD_SCKT_ERR_IS_EINTR_(err) ||
          EBUSY == err)
        {
          mhd_assert (SSIZE_MAX >= sent_bytes);
          if (0 != sent_bytes)
            return (ssize_t)sent_bytes;

          return MHD_ERR_AGAIN_;
        }
      /* Some unrecoverable error. Possibly file FD is not suitable
       * for sendfile(). Retry with standard send(). */
      connection->resp_sender = MHD_resp_sender_std;
      return MHD_ERR_AGAIN_;
    }
  mhd_assert (0 < sent_bytes);
  mhd_assert (SSIZE_MAX >= sent_bytes);
  ret = (ssize_t)sent_bytes;
#elif defined(HAVE_DARWIN_SENDFILE)
  len = (off_t)send_size; /* chunk always fit */
  if (0 != sendfile (file_fd,
                     connection->socket_fd,
                     (off_t) offsetu64,
                     &len,
                     NULL,
                     0))
    {
      const int err = MHD_socket_get_error_();
      if (MHD_SCKT_ERR_IS_EAGAIN_(err) ||
          MHD_SCKT_ERR_IS_EINTR_(err))
        {
          mhd_assert (0 <= len);
          mhd_assert (SSIZE_MAX >= len);
          mhd_assert (send_size >= (size_t)len);
          if (0 != len)
            return (ssize_t)len;

          return MHD_ERR_AGAIN_;
        }
      if (ENOTCONN == err ||
          EPIPE == err)
        return MHD_ERR_CONNRESET_;
      if (ENOTSUP == err ||
          EOPNOTSUPP == err)
        { /* This file FD is not suitable for sendfile().
           * Retry with standard send(). */
          connection->resp_sender = MHD_resp_sender_std;
          return MHD_ERR_AGAIN_;
        }
      return MHD_ERR_BADF_; /* Return hard error. */
    }
  mhd_assert (0 <= len);
  mhd_assert (SSIZE_MAX >= len);
  mhd_assert (send_size >= (size_t)len);
  ret = (ssize_t)len;
#endif /* HAVE_FREEBSD_SENDFILE */

  /* ! could be avoided by redefining the variable. */
  have_cork = ! connection->sk_tcp_nodelay_on;

#ifdef MSG_MORE
  have_more = true;
#else
  have_more = false;
#endif

#if TCP_NODELAY
  use_corknopush = false;
#elif TCP_CORK
  use_corknopush = true;
#elif TCP_NOPUSH
  use_corknopush = true;
#endif

#ifdef HTTPS_SUPPORT
  using_tls = (0 != (connection->daemon->options & MHD_USE_TLS));
#endif

#if TCP_CORK
  /* When we have CORK, we can have NODELAY on the same system,
   * at least since Linux 2.2 and both can be combined since
   * Linux 2.5.71. For more details refer to tcp(7) on Linux.
   * No other system in 2019-06 has TCP_CORK. */
  if ((! using_tls) && (use_corknopush) && (have_cork && ! want_cork))
    {
      if (0 == setsockopt (connection->socket_fd,
                           IPPROTO_TCP,
                           TCP_CORK,
                           (const void *) &off_val,
                           sizeof (off_val)))
      {
        connection->sk_tcp_nodelay_on = true;
      }
      else if (0 == setsockopt (connection->socket_fd,
                                IPPROTO_TCP,
                                TCP_NODELAY,
                                (const void *) &on_val,
                                sizeof (on_val)))
      {
        connection->sk_tcp_nodelay_on = true;
      }
    }
#elif TCP_NOPUSH
  /* TCP_NOPUSH on FreeBSD is equal to cork on Linux, with the
   * exception that we know that TCP_NOPUSH will definitely
   * exist and we can disregard TCP_NODELAY unless requested. */
  if ((! using_tls) && (use_corknopush) && (have_cork && ! want_cork))
    {
      MHD_send_socket_state_nopush_ (connection, true, false);
      /*
      if (0 == setsockopt (connection->socket_fd,
                           IPPROTO_TCP,
                           TCP_NOPUSH,
                           (const void *) &on_val,
                           sizeof (on_val)))
        {
          connection->sk_tcp_nodelay_on = false;
        }
      */
    }
#endif

  return ret;
}
#endif /* _MHD_HAVE_SENDFILE */