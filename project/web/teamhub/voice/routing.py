from django.urls import re_path
from . import consumers

voice_urlpatterns = [
    re_path(r"^ws/voice/?$", consumers.VoiceConsumer.as_asgi()),
]
