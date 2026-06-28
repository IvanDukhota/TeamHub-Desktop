import os

from django.core.asgi import get_asgi_application
from channels.routing import ProtocolTypeRouter, URLRouter

os.environ.setdefault('DJANGO_SETTINGS_MODULE', 'teamhub.settings')

django_asgi_app = get_asgi_application()

from collab.routing import collab_urlpatterns
from voice.routing import voice_urlpatterns

application = ProtocolTypeRouter(
    {
        'http': django_asgi_app,
        'websocket': URLRouter(collab_urlpatterns + voice_urlpatterns),
    }
)
