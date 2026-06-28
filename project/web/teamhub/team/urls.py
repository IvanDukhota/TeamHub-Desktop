from django.urls import path

from .views import (
    TeamDetailView,
    TeamEditingSessionReportView,
    TeamEditingSessionsView,
    TeamListCreateView,
    TeamMemberDetailView,
    TeamMembersView,
    TeamRolesView,
    TeamVoiceRoomDetailView,
    TeamVoiceRoomsView,
)

urlpatterns = [
    path('', TeamListCreateView.as_view(), name='team-list'),
    path('<uuid:pk>/', TeamDetailView.as_view(), name='team-detail'),
    path('<uuid:pk>/members/', TeamMembersView.as_view(), name='team-members'),
    path('<uuid:pk>/members/<int:user_id>/', TeamMemberDetailView.as_view(), name='team-member-detail'),
    path('<uuid:pk>/roles/', TeamRolesView.as_view(), name='team-roles'),
    path('<uuid:pk>/rooms/', TeamVoiceRoomsView.as_view(), name='team-voice-rooms'),
    path('<uuid:pk>/rooms/<uuid:room_pk>/', TeamVoiceRoomDetailView.as_view(), name='team-voice-room-detail'),
    path('<uuid:pk>/editing-sessions/', TeamEditingSessionsView.as_view(), name='team-editing-sessions'),
    path(
        '<uuid:pk>/editing-sessions/<uuid:session_id>/report/',
        TeamEditingSessionReportView.as_view(),
        name='team-editing-session-report',
    ),
]
