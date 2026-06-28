from django.urls import path

from .views import LoginView, LogoutView, MyRoomsView, MyTeamsView, ProfileView, RegisterView

urlpatterns = [
    path('register/', RegisterView.as_view(), name='auth-register'),
    path('login/', LoginView.as_view(), name='auth-login'),
    path('logout/', LogoutView.as_view(), name='auth-logout'),
    path('profile/', ProfileView.as_view(), name='auth-profile'),
    path('me/teams/', MyTeamsView.as_view(), name='auth-my-teams'),
    path('me/rooms/', MyRoomsView.as_view(), name='auth-my-rooms'),
]
