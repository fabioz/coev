import socket, errno, urlparse, urllib, posixpath, sys
import coev, thread
from BaseHTTPServer import BaseHTTPRequestHandler

SocketErrors = (socket.error,)

__version__ = '0.3'

class CoevWSGIServer(object):
    """ coev server almost compatible with BaseHTTPRequestHandler """
    
    address_family = socket.AF_INET
    socket_type = socket.SOCK_STREAM
    request_queue_size = 5
    allow_reuse_address = True
    accept_timeout = 5.0
    max_request_size = 1048576 # 1M
    
    def __init__(self, wsgi_application, server_address, 
                        RequestHandlerClass = None,
                        request_queue_size = 500, 
                        iop_timeout = 5,
                        wsgi_timeout = 5 ):
        self.server_address = server_address
        self.request_queue_size = request_queue_size
        self.RequestHandlerClass = RequestHandlerClass
        self.__serving = False
        self.iop_timeout = iop_timeout
        self.wsgi_application = wsgi_application
        self.wsgi_timeout = wsgi_timeout

    def bind(self):
        self.socket = socket.socket(self.address_family, self.socket_type)
        self.socket.setblocking(False)
        if self.allow_reuse_address:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(self.server_address)
        self.server_address = self.socket.getsockname()
        host, self.server_port = self.server_address[:2]
        self.server_name = socket.getfqdn(host)
        self.socket.listen(self.request_queue_size)

    def unbind(self):
        self.socket.close()

    def serve(self):
        self.__serving = True
        fd = self.socket.fileno()
        while self.__serving:
            while True: # while we're accepting stuff
                try:
                    (so, ai) = self.socket.accept()
                except socket.error, e:
                    if e.errno == 11:
                        break
                so.setblocking(False);
                handler = thread.start_new_thread(self.handler, (so, ai, self, None))
            # wait for connects
            try:
                coev.wait(fd, coev.READ, self.accept_timeout)
            except (coev.WaitAbort, coev.Timeout):
                pass
            

        self.__serving = False

    def handler(self, *args):
        self.RequestHandlerClass(*args)
        

    def shutdown(self):
        self.__serving = False

class ContinueHook(object):
    """
    When a client request includes a 'Expect: 100-continue' header, then
    it is the responsibility of the server to send 100 Continue when it
    is ready for the content body.  This allows authentication, access
    levels, and other exceptions to be detected *before* bandwith is
    spent on the request body.

    This is a rfile wrapper that implements this functionality by
    sending 100 Continue to the client immediately after the user
    requests the content via a read() operation on the rfile stream.
    After this response is sent, it becomes a pass-through object.
    """

    def __init__(self, rfile, write):
        self._ContinueFile_rfile = rfile
        self._ContinueFile_write = write
        for attr in ('close', 'closed', 'fileno', 'flush',
                     'mode', 'bufsize', 'softspace'):
            if hasattr(rfile, attr):
                setattr(self, attr, getattr(rfile, attr))
        for attr in ('read', 'readline', 'readlines'):
            if hasattr(rfile, attr):
                setattr(self, attr, getattr(self, '_ContinueFile_' + attr))

    def _ContinueFile_send(self):
        self._ContinueFile_write("HTTP/1.1 100 Continue\r\n\r\n")
        rfile = self._ContinueFile_rfile
        for attr in ('read', 'readline', 'readlines'):
            if hasattr(rfile, attr):
                setattr(self, attr, getattr(rfile, attr))

    def _ContinueFile_read(self, size=-1):
        self._ContinueFile_send()
        return self._ContinueFile_rfile.readline(size)

    def _ContinueFile_readline(self, size=-1):
        self._ContinueFile_send()
        return self._ContinueFile_rfile.readline(size)

    def _ContinueFile_readlines(self, sizehint=0):
        self._ContinueFile_send()
        return self._ContinueFile_rfile.readlines(sizehint)

class WSGIHandlerMixin:
    """
    WSGI mix-in for HTTPRequestHandler

    This class is a mix-in to provide WSGI functionality to any
    HTTPRequestHandler derivative (as provided in Python's BaseHTTPServer).
    This assumes a ``wsgi_application`` handler on ``self.server``.
    
    Ripped from paste.httpserver for the following reasons:
        -- LimitedLengthFile bullshit removed.
    
    """
    lookup_addresses = True

    def log_request(self, *args, **kwargs):
        """ disable success request logging

        Logging transactions should not be part of a WSGI server,
        if you want logging; look at paste.translogger
        """
        pass

    def log_message(self, *args, **kwargs):
        """ disable error message logging

        Logging transactions should not be part of a WSGI server,
        if you want logging; look at paste.translogger
        """
        pass

    def version_string(self):
        """ behavior that BaseHTTPServer should have had """
        if not self.sys_version:
            return self.server_version
        else:
            return self.server_version + ' ' + self.sys_version

    def wsgi_write_chunk(self, chunk):
        """
        Write a chunk of the output stream; send headers if they
        have not already been sent.
        """
        if not self.wsgi_headers_sent and not self.wsgi_curr_headers:
            raise RuntimeError(
                "Content returned before start_response called")
        if not self.wsgi_headers_sent:
            self.wsgi_headers_sent = True
            (status, headers) = self.wsgi_curr_headers
            code, message = status.split(" ", 1)
            self.send_response(int(code), message)
            #
            # HTTP/1.1 compliance; either send Content-Length or
            # signal that the connection is being closed.
            #
            send_close = True
            for (k, v) in  headers:
                lk = k.lower()
                if 'content-length' == lk:
                    send_close = False
                if 'connection' == lk:
                    if 'close' == v.lower():
                        self.close_connection = 1
                        send_close = False
                self.send_header(k, v)
            if send_close:
                self.close_connection = 1
                self.send_header('Connection', 'close')

            self.end_headers()
        self.wfile.write(chunk)

    def wsgi_start_response(self, status, response_headers, exc_info=None):
        if exc_info:
            try:
                if self.wsgi_headers_sent:
                    raise exc_info[0], exc_info[1], exc_info[2]
                else:
                    # In this case, we're going to assume that the
                    # higher-level code is currently handling the
                    # issue and returning a resonable response.
                    # self.log_error(repr(exc_info))
                    pass
            finally:
                exc_info = None
        elif self.wsgi_curr_headers:
            assert 0, "Attempt to set headers a second time w/o an exc_info"
        self.wsgi_curr_headers = (status, response_headers)
        return self.wsgi_write_chunk

    def wsgi_setup(self, environ=None):
        """
        Setup the member variables used by this WSGI mixin, including
        the ``environ`` and status member variables.

        After the basic environment is created; the optional ``environ``
        argument can be used to override any settings.
        """

        (scheme, netloc, path, query, fragment) = urlparse.urlsplit(self.path)
        path = urllib.unquote(path)
        endslash = path.endswith('/')
        path = posixpath.normpath(path)
        if endslash and path != '/':
            # Put the slash back...
            path += '/'
        (server_name, server_port) = self.server.server_address

        rfile = self.rfile
        if 'HTTP/1.1' == self.protocol_version and \
                '100-continue' == self.headers.get('Expect','').lower():
            rfile = ContinueHook(rfile, self.wfile.write)

        remote_address = self.client_address[0]
        self.wsgi_environ = {
                'wsgi.version': (1,0)
               ,'wsgi.url_scheme': 'http'
               ,'wsgi.input': rfile
               ,'wsgi.errors': sys.stderr
               ,'wsgi.multithread': True
               ,'wsgi.multiprocess': False
               ,'wsgi.run_once': False
               # CGI variables required by PEP-333
               ,'REQUEST_METHOD': self.command
               ,'SCRIPT_NAME': '' # application is root of server
               ,'PATH_INFO': path
               ,'QUERY_STRING': query
               ,'CONTENT_TYPE': self.headers.get('Content-Type', '')
               ,'CONTENT_LENGTH': self.headers.get('Content-Length', '0')
               ,'SERVER_NAME': server_name
               ,'SERVER_PORT': str(server_port)
               ,'SERVER_PROTOCOL': self.request_version
               # CGI not required by PEP-333
               ,'REMOTE_ADDR': remote_address
               }
        if scheme:
            self.wsgi_environ['paste.httpserver.proxy.scheme'] = scheme
        if netloc:
            self.wsgi_environ['paste.httpserver.proxy.host'] = netloc

        if self.lookup_addresses:
            # @@: make lookup_addreses actually work, at this point
            #     it has been address_string() is overriden down in
            #     file and hence is a noop
            if remote_address.startswith("192.168.") \
            or remote_address.startswith("10.") \
            or remote_address.startswith("172.16."):
                pass
            else:
                address_string = None # self.address_string()
                if address_string:
                    self.wsgi_environ['REMOTE_HOST'] = address_string

        for k, v in self.headers.items():
            key = 'HTTP_' + k.replace("-","_").upper()
            if key in ('HTTP_CONTENT_TYPE','HTTP_CONTENT_LENGTH'):
                continue
            self.wsgi_environ[key] = ','.join(self.headers.getheaders(k))

        if environ:
            assert isinstance(environ, dict)
            self.wsgi_environ.update(environ)
            if 'on' == environ.get('HTTPS'):
                self.wsgi_environ['wsgi.url_scheme'] = 'https'

        self.wsgi_curr_headers = None
        self.wsgi_headers_sent = False

    def wsgi_connection_drop(self, exce, environ=None):
        """
        Override this if you're interested in socket exceptions, such
        as when the user clicks 'Cancel' during a file download.
        """
        pass

    def wsgi_execute(self, environ=None):
        """
        Invoke the server's ``wsgi_application``.
        """

        self.wsgi_setup(environ)

        try:
            result = self.server.wsgi_application(self.wsgi_environ,
                                                  self.wsgi_start_response)
            try:
                for chunk in result:
                    self.wsgi_write_chunk(chunk)
                if not self.wsgi_headers_sent:
                    self.wsgi_write_chunk('')
            finally:
                if hasattr(result,'close'):
                    result.close()
                result = None
        except socket.error, exce:
            self.wsgi_connection_drop(exce, environ)
            return
        except:
            if not self.wsgi_headers_sent:
                error_msg = "Internal Server Error\n"
                self.wsgi_curr_headers = (
                    '500 Internal Server Error',
                    [('Content-type', 'text/plain'),
                     ('Content-length', str(len(error_msg)))])
                self.wsgi_write_chunk("Internal Server Error\n")
            raise


class CoevWSGIHandler(WSGIHandlerMixin, BaseHTTPRequestHandler):
    server_version = 'CoevWSGIServer/' + __version__

    def __init__(self, request, client_address, server, handling_coroutine):
        self.request = request
        self.client_address = client_address
        self.server = server
        self.handling_coroutine = handling_coroutine
        
        self.connection = self.request
        self.rfile = self.wfile = coev.socketfile(self.request.fileno(), 
            self.server.iop_timeout, self.server.max_request_size)
        try:
            self.handle()
        finally:
            self.request.close()
    
    def watch_handling(self, timeout):
        coev.sleep(timeout)
        self.handling_coroutine.throw(coev.Exit)

    def handle_one_request(self):
        try:
            self.raw_requestline = self.rfile.readline(1024)
        except coev.Timeout:
            self.close_connection = 1
            return
        if not self.raw_requestline:
            self.close_connection = 1
            return
        if not self.parse_request(): # An error code has been sent, just exit
            self.close_connection = 1
            return
        self.wsgi_execute()

    def handle(self):
        # don't bother logging disconnects while handling a request
        try:
            self.close_connection = 1

            self.handle_one_request()
            while not self.close_connection:
                self.handle_one_request()
        except SocketErrors, exce:
            self.wsgi_connection_drop(exce)
        except coev.SocketError, exce:
            self.wsgi_connection_drop(exce)

    def address_string(self):
        """Return the client address formatted for logging.

        This is overridden so that no hostname lookup is done.
        """
        return ''

def serve(application, host=None, port=None, handler=None, ssl_pem=None,
          ssl_context=None, server_version=None, protocol_version=None,
          start_loop=True, daemon_threads=None, socket_timeout=4.2,
          use_threadpool=None, threadpool_workers=10,
          threadpool_options=None, request_queue_size=10, response_timeout=42.23):
          
    assert not handler, "foreign handlers are prohibited"
    assert not ssl_context, "SSL/TLS not supported"
    assert not ssl_pem, "SSL/TLS not supported"
    assert not use_threadpool, "threads are evil"
    #assert converters.asbool(start_loop), "WTF?"
    """
    Serves your ``application`` over HTTP via WSGI interface

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

    host = host or '127.0.0.1'
    if not port:
        if ':' in host:
            host, port = host.split(':', 1)
        else:
            port = 8080
    server_address = (host, int(port))

    handler = CoevWSGIHandler
    if server_version:
        handler.server_version = server_version
        handler.sys_version = None
    if protocol_version:
        assert protocol_version in ('HTTP/0.9', 'HTTP/1.0', 'HTTP/1.1')
        handler.protocol_version = protocol_version

    server = CoevWSGIServer(application, 
                server_address, handler,
                request_queue_size, float(socket_timeout),
                float(response_timeout) )

    protocol = 'http'
    host, port = server.server_address
    if host == '0.0.0.0':
        print 'serving on 0.0.0.0:%s view at %s://127.0.0.1:%s' % \
            (port, protocol, port)
    else:
        print "serving on %s://%s:%s" % (protocol, host, port)
    try:
        server.bind()
        server.serve()
    except KeyboardInterrupt:
        # allow CTRL+C to shutdown
        pass
    finally:
        server.unbind()

def thunk(app, args):
    try:
        serve(app, **args)
    except Exception, e:
        print "aborting"
        sys.exit(0)


# For paste.deploy server instantiation (egg:Egste#http)
# Note: this gets a separate function because it has to expect string
# arguments (though that's not much of an issue yet, ever?)
def server_runner(wsgi_app, global_conf, **kwargs):
    sys.setcheckinterval(10000000)
    
    from paste.deploy.converters import asbool
    for name in ['port', 'socket_timeout']:
        if name in kwargs:
            kwargs[name] = int(kwargs[name])
    if ('error_email' not in kwargs
        and 'error_email' in global_conf):
        kwargs['error_email'] = global_conf['error_email']
    
    server = thread.start_new_thread(thunk, (wsgi_app, kwargs))
    coev.scheduler()


if __name__ == '__main__':
    sys.setcheckinterval(10000000)
    coev.setdebug(True, coev.CDF_COEV | coev.CDF_COEV_DUMP |coev.CDF_RUNQ_DUMP | coev.CDF_NBUF)
    #coev.setdebug(False, 0)
    #coev.setdebug(True, 0xf | coev.CDF_NBUF)
    sys.excepthook = sys.__excepthook__
    from paste.wsgilib import dump_environ
    kwargs = { 'server_version': "Wombles/1.0", 'protocol_version': "HTTP/1.1", 'port': "8888" }

    server = thread.start_new_thread(thunk, (dump_environ, kwargs))
    coev.scheduler()
    
    
