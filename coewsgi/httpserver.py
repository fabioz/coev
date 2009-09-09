import socket, errno
import SocketServer
from BaseHTTPServer import BaseHTTPRequestHandler
import coev
from paste.httpserver import WSGIHandlerMixin
from paste.util import converters

SocketErrors = (socket.error,)

class CoEvHTTPServer(SocketServer.TCPServer):
    
    allow_reuse_address = 1    # Seems to make sense in testing environment
    def __init__(self, server_address, RequestHandlerClass):
        """Constructor.  May be extended, do not override."""
        SocketServer.TCPServer.__init__(self, server_address, RequestHandlerClass)
        self.socket.setblocking(0)
        
    def get_request(self):
        while True:
            try:
                #print "get_request(): waiting"
                coev.wait(self.socket.fileno(), coev.READ, self.io_timeout * 10)
            except coev.Timeout:
                #print "get_request(): timeout"
                continue
            break
        rv = self.socket.accept()
        #print "get_request(): returning %r" % (rv,)
        return rv

    def server_bind(self):
        """Override server_bind to store the server name."""
        SocketServer.TCPServer.server_bind(self)
        host, port = self.socket.getsockname()[:2]
        self.server_name = socket.getfqdn(host)
        self.server_port = port

    def process_request_eg(self, request, client_address):
        """Same as in BaseServer but as a coroutine.

        In addition, exception handling is done here.

        """
        try:
            self.finish_request(request, client_address)
            self.close_request(request)
        except:
            self.handle_error(request, client_address)
            self.close_request(request)

    def process_request(self, request, client_address):
        """Start a new thread to process the request."""
        #print "process_request: creating coro"
        eg = coev.coroutine(self.process_request_eg)
        #print "process_request: switching"
        eg.switch(request, client_address)


class WSGIServer(CoEvHTTPServer):
    def __init__(self, wsgi_application, server_address,
                 RequestHandlerClass=None, ssl_context=None, io_timeout=5.0):
        CoEvHTTPServer.__init__(self, server_address,
                                  RequestHandlerClass)
        self.wsgi_application = wsgi_application
        self.wsgi_socket_timeout = None
        self.io_timeout = io_timeout

class CoEvFile(object):
    def __init__(self, sock, mode, bufsize, io_timeout=5.0):
        sock.setblocking(0)
        self.sock = sock
        self.io_timeout = io_timeout
        
        if mode in ('r', 'rb'):
            self.mode = coev.READ
        elif mode in ('w', 'wb'):
            self.mode = coev.WRITE
        else:
            raise ValueError("Unknown/unsupported mode '%r'", mode)
        self.bufsize = bufsize
        self.buffer = ''
        self.closed = False
        
    def read(self, n):
        unread = n
        rv = self.buffer[:max(len(self.buffer), n)]
        unread -= max(len(self.buffer), n)
        self.buffer = self.buffer[max(len(self.buffer), n):]
        if unread < 1:
            return rv
        
        while True:
            try:
                data = self.sock.recv(self.bufsize - len(self.buffer))
            except socket.error, e:
                if e.args[0] == errno.EAGAIN:
                    coev.wait(self.sock.fileno(), coev.READ, self.io_timeout)
            if len(data) == 0:
                break
            self.buffer += data
            
            if len(self.buffer) >= unread:
                rv += self.buffer[:unread]
                self.buffer = self.buffer[unread:]
                return rv
        
    def readline(self):
        while True:
            lineandrest = self.buffer.split('\n', 1)
            if len(lineandrest) == 2:
                self.buffer = lineandrest[1]
                return lineandrest[0]
            try:
                data = self.sock.recv(1024)
            except socket.error, e:
                if e.args[0] == errno.EAGAIN:
                    coev.wait(self.sock.fileno(), coev.READ, self.io_timeout)
                    continue
                    
            if len(data) == 0:
                self.close()
                return None
                
            self.buffer += data

    def _sendbuf(self):
        while len(self.buffer) > 0:
            try:
                nwritten = self.sock.send(self.buffer)
                self.buffer = self.buffer[nwritten:]
            except socket.error, e:
                if e.args[0] == errno.EAGAIN:
                    coev.wait(self.sock.fileno(), coev.WRITE, self.io_timeout)
                elif e.args[0] == errno.EPIPE:
                    self.closed = True
                    self.sock = None
                    return
            except AttributeError:
                self.closed = True
                return
    
    def write(self, data):
        self.buffer += data
        if len(self.buffer) < self.bufsize:
            return
        self._sendbuf()

    def flush(self):
        assert self.mode == coev.WRITE, "Can't flush rfile"
        self._sendbuf()

    def close(self):
        self.closed = True
        self.sock = None
    

class CoEvWSGIHandler(WSGIHandlerMixin, BaseHTTPRequestHandler):
    """
    A WSGI handler that overrides POST, GET and HEAD to delegate
    requests to the server's ``wsgi_application``.
    """
    __version__ = '0.1'
    server_version = 'CoEvWSGIServer/' + __version__

    """ override the hell out of StreamRequestHandler """

    def setup(self):
        self.connection = self.request
        self.rfile = CoEvFile(self.connection, 'rb', self.rbufsize)
        self.wfile = CoEvFile(self.connection, 'wb', self.wbufsize)

    def finish(self):
        if not self.wfile.closed:
            self.wfile.flush()
        self.wfile.close()
        self.rfile.close()

    def handle_one_request(self):
        """Handle a single HTTP request.

        You normally don't need to override this method; see the class
        __doc__ string for information on how to handle specific HTTP
        commands such as GET and POST.

        """
        self.raw_requestline = self.rfile.readline()
        if not self.raw_requestline:
            self.close_connection = 1
            return
        if not self.parse_request(): # An error code has been sent, just exit
            return
        self.wsgi_execute()

    def handle(self):
        # don't bother logging disconnects while handling a request
        try:
            BaseHTTPRequestHandler.handle(self)
        except SocketErrors, exce:
            self.wsgi_connection_drop(exce)

    def address_string(self):
        """Return the client address formatted for logging.
        
        This is overridden so that no hostname lookup is done.
        """
        return ''

# dunno what to make of this in context of coroutines
#    def get_request(self):
#        # If there is a socket_timeout, set it on the accepted
#        (conn,info) = CoEvHTTPServer.get_request(self)
#        if self.wsgi_socket_timeout:
#            conn.settimeout(self.wsgi_socket_timeout)
#        return (conn, info)


def serve(application, host=None, port=None, handler=None, ssl_pem=None,
          ssl_context=None, server_version=None, protocol_version=None,
          start_loop=True, socket_timeout=None, coevdebug = (1,0) ):
    """
    Serves your ``application`` over HTTP(S) via WSGI interface

    ``host``

        This is the ipaddress to bind to (or a hostname if your
        nameserver is properly configured).  This defaults to
        127.0.0.1, which is not a public interface.

    ``port``

        The port to run on, defaults to 8080 for HTTP, or 4443 for
        HTTPS. This can be a string or an integer value.

    ``handler``

        This is the HTTP request handler to use, it defaults to
        ``WSGIHandler`` in this module.

    ``ssl_pem``

        This an optional SSL certificate file (via OpenSSL). You can
        supply ``*`` and a development-only certificate will be
        created for you, or you can generate a self-signed test PEM
        certificate file as follows::

            $ openssl genrsa 1024 > host.key
            $ chmod 400 host.key
            $ openssl req -new -x509 -nodes -sha1 -days 365  \\
                          -key host.key > host.cert
            $ cat host.cert host.key > host.pem
            $ chmod 400 host.pem

    ``ssl_context``

        This an optional SSL context object for the server.  A SSL
        context will be automatically constructed for you if you supply
        ``ssl_pem``.  Supply this to use a context of your own
        construction.

    ``server_version``

        The version of the server as reported in HTTP response line. This
        defaults to something like "PasteWSGIServer/0.5".  Many servers
        hide their code-base identity with a name like 'Amnesiac/1.0'

    ``protocol_version``

        This sets the protocol used by the server, by default
        ``HTTP/1.0``. There is some support for ``HTTP/1.1``, which
        defaults to nicer keep-alive connections.  This server supports
        ``100 Continue``, but does not yet support HTTP/1.1 Chunked
        Encoding. Hence, if you use HTTP/1.1, you're somewhat in error
        since chunked coding is a mandatory requirement of a HTTP/1.1
        server.  If you specify HTTP/1.1, every response *must* have a
        ``Content-Length`` and you must be careful not to read past the
        end of the socket.

    ``start_loop``

        This specifies if the server loop (aka ``server.serve_forever()``)
        should be called; it defaults to ``True``.

    ``socket_timeout``

        This specifies the maximum amount of time that a connection to a
        given client will be kept open.  At this time, it is a rude
        disconnect, but at a later time it might follow the RFC a bit
        more closely.

    """
    coev.setdebug(*coevdebug)
    is_ssl = False
    if ssl_pem or ssl_context:
        assert SSL, "pyOpenSSL is not installed"
        is_ssl = True
        port = int(port or 4443)
        if not ssl_context:
            if ssl_pem == '*':
                ssl_context = _auto_ssl_context()
            else:
                ssl_context = SSL.Context(SSL.SSLv23_METHOD)
                ssl_context.use_privatekey_file(ssl_pem)
                ssl_context.use_certificate_file(ssl_pem)

    host = host or '127.0.0.1'
    if not port:
        if ':' in host:
            host, port = host.split(':', 1)
        else:
            port = 8080
    server_address = (host, int(port))

    if not handler:
        handler = CoEvWSGIHandler
    else:
        raise ValueError("must use CoEvWSGIHandler")
    if server_version:
        handler.server_version = server_version
        handler.sys_version = None
    if protocol_version:
        assert protocol_version in ('HTTP/0.9', 'HTTP/1.0', 'HTTP/1.1')
        handler.protocol_version = protocol_version

    server = WSGIServer(application, server_address, handler, ssl_context)

    if socket_timeout:
        server.wsgi_socket_timeout = int(socket_timeout)

    if converters.asbool(start_loop):
        protocol = is_ssl and 'https' or 'http'
        host, port = server.server_address
        if host == '0.0.0.0':
            print 'serving on 0.0.0.0:%s view at %s://127.0.0.1:%s' % \
                (port, protocol, port)
        else:
            print "serving on %s://%s:%s" % (protocol, host, port)
        try:
            
            server_coro = coev.coroutine(server.serve_forever)
            server_coro.switch()
            coev.scheduler()
        except KeyboardInterrupt:
            # allow CTRL+C to shutdown
            pass
    else:
        raise ValueError("start_loop=False not supported")
        
    return server

# For paste.deploy server instantiation (egg:Egste#http)
# Note: this gets a separate function because it has to expect string
# arguments (though that's not much of an issue yet, ever?)
def server_runner(wsgi_app, global_conf, **kwargs):
    from paste.deploy.converters import asbool
    for name in ['port', 'socket_timeout']:
        if name in kwargs:
            kwargs[name] = int(kwargs[name])
    for name, value in kwargs.items():
        if name.startswith('threadpool_'):
            del kwargs[name]
    if ('error_email' not in kwargs
        and 'error_email' in global_conf):
        kwargs['error_email'] = global_conf['error_email']
    serve(wsgi_app, **kwargs)

