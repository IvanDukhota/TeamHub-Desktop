from django.urls import re_path
from . import consumers

collab_urlpatterns = [
    re_path(r"^ws/collab/(?P<room>[^/]+)/?$", consumers.CollabConsumer.as_asgi()),
]
