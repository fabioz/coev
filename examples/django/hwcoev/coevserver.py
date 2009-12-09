import coewsgi.httpserver as server
import os, sys

sys.path.append(os.path.dirname((os.getcwd()))) 
os.environ['DJANGO_SETTINGS_MODULE'] = 'hwcoev.settings'

import django.core.handlers.wsgi

application = django.core.handlers.wsgi.WSGIHandler()

server.serve(application)

