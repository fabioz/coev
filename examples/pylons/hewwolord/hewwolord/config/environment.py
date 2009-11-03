"""Pylons environment configuration"""
import os

from genshi.template import TemplateLoader
from pylons import config
from sqlalchemy import engine_from_config
from paste.deploy.converters import *

import hewwolord.lib.app_globals as app_globals
import hewwolord.lib.helpers
from hewwolord.config.routing import make_map
from hewwolord.model import init_model

def load_environment(global_conf, app_conf):
    """Configure the Pylons environment via the ``pylons.config``
    object
    """
    # Pylons paths
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    paths = dict(root=root,
                 controllers=os.path.join(root, 'controllers'),
                 static_files=os.path.join(root, 'public'),
                 templates=[os.path.join(root, 'templates')])

    # Initialize config with the basic options
    config.init_app(global_conf, app_conf, package='hewwolord', paths=paths)

    config['routes.map'] = make_map()
    config['pylons.app_globals'] = app_globals.Globals()
    config['pylons.h'] = hewwolord.lib.helpers

    # Create the Genshi TemplateLoader
    config['pylons.app_globals'].genshi_loader = TemplateLoader(
        paths['templates'], auto_reload=True)

    # Setup the SQLAlchemy database engine
    if 'sqlalchemy.module' in config:
        config['sqlalchemy.module'] = __import__(config['sqlalchemy.module'])

    engine = engine_from_config(config, 'sqlalchemy.')
        
    try:
        import coev

        flagdict = {
            'coev.debug.lib.coev': coev.CDF_COEV,
            'coev.debug.lib.coev.dump': coev.CDF_COEV_DUMP,
            'coev.debug.lib.colock': coev.CDF_COLOCK,
            'coev.debug.lib.colock.dump': coev.CDF_COLOCK_DUMP,
            'coev.debug.lib.nbuf': coev.CDF_NBUF,
            'coev.debug.lib.nbuf.dump': coev.CDF_NBUF_DUMP,
            'coev.debug.lib.runq.dump': coev.CDF_RUNQ_DUMP,
            'coev.debug.lib.stack': coev.CDF_STACK,
            'coev.debug.lib.stack.dump': coev.CDF_STACK_DUMP }
        
        lib_debug_flags = 0
        for f in flagdict:
            if asbool(config.get(f, False)):
                lib_debug_flags |= flagdict[f]

        coev.setdebug(  asbool(config.get('coev.debug.module', False)),
                        lib_debug_flags )
                        
        import thread
        thread.stack_size(int(config.get('coev.stack.size', 2 * 1024 * 1024)))
    except ImportError:
        pass    
    
    mcservers = aslist(config['memcache.servers'])
    mcdebug = asbool(config['memcache.debug'])
    init_model(engine, mcservers, mcdebug)

    # CONFIGURATION OPTIONS HERE (note: all config options will override
    # any Pylons config options)
