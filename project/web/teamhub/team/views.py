from django.utils import timezone
from rest_framework import generics, status
from rest_framework.exceptions import NotFound, PermissionDenied
from rest_framework.permissions import IsAuthenticated
from rest_framework.response import Response
from rest_framework.views import APIView

from .models import EditingReport, EditingSession, Team, TeamMember, TeamRole, VoiceRoom
from .serializers import (
    EditingReportSerializer,
    EditingSessionSerializer,
    TeamDetailSerializer,
    TeamMemberSerializer,
    TeamRoleSerializer,
    TeamSerializer,
    VoiceRoomSerializer,
)


def _get_team_or_403(pk, user):
    try:
        team = Team.objects.get(pk=pk)
    except Team.DoesNotExist:
        from rest_framework.exceptions import NotFound
        raise NotFound('Team not found.')
    if not team.members.filter(user=user).exists():
        raise PermissionDenied('You are not a member of this team.')
    return team


def _require_admin(team, user):
    member = team.members.select_related('role').filter(user=user).first()
    is_creator = team.created_by == user
    is_role_admin = member and member.role and member.role.is_admin
    if not (is_creator or is_role_admin):
        raise PermissionDenied('Admin access required.')


class TeamListCreateView(generics.ListCreateAPIView):
    permission_classes = [IsAuthenticated]
    serializer_class = TeamSerializer

    def get_queryset(self):
        return Team.objects.filter(members__user=self.request.user).distinct()

    def get_serializer_class(self):
        return TeamSerializer

    def perform_create(self, serializer):
        serializer.save()


class TeamDetailView(APIView):
    permission_classes = [IsAuthenticated]

    def get(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        return Response(TeamDetailSerializer(team, context={'request': request}).data)

    def patch(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        _require_admin(team, request.user)
        serializer = TeamSerializer(team, data=request.data, partial=True, context={'request': request})
        serializer.is_valid(raise_exception=True)
        serializer.save()
        return Response(serializer.data)

    def put(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        _require_admin(team, request.user)
        serializer = TeamSerializer(team, data=request.data, context={'request': request})
        serializer.is_valid(raise_exception=True)
        serializer.save()
        return Response(serializer.data)

    def delete(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        if team.created_by != request.user:
            raise PermissionDenied('Only the team creator can delete the team.')
        team.delete()
        return Response(status=status.HTTP_204_NO_CONTENT)


class TeamMembersView(APIView):
    permission_classes = [IsAuthenticated]

    def get(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        members = team.members.select_related('user', 'role').all()
        return Response(TeamMemberSerializer(members, many=True).data)

    def post(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        _require_admin(team, request.user)
        serializer = TeamMemberSerializer(data=request.data)
        serializer.is_valid(raise_exception=True)
        serializer.save(team=team)
        return Response(serializer.data, status=status.HTTP_201_CREATED)


class TeamMemberDetailView(APIView):
    permission_classes = [IsAuthenticated]

    def delete(self, request, pk, user_id):
        team = _get_team_or_403(pk, request.user)
        _require_admin(team, request.user)
        member = team.members.filter(user_id=user_id).first()
        if not member:
            return Response({'detail': 'Member not found.'}, status=status.HTTP_404_NOT_FOUND)
        if member.user == team.created_by:
            raise PermissionDenied('Cannot remove the team creator.')
        member.delete()
        return Response(status=status.HTTP_204_NO_CONTENT)


class TeamRolesView(generics.ListCreateAPIView):
    permission_classes = [IsAuthenticated]
    serializer_class = TeamRoleSerializer

    def get_queryset(self):
        team = _get_team_or_403(self.kwargs['pk'], self.request.user)
        return team.roles.all()

    def perform_create(self, serializer):
        team = _get_team_or_403(self.kwargs['pk'], self.request.user)
        _require_admin(team, self.request.user)
        serializer.save(team=team)


class TeamVoiceRoomsView(APIView):
    permission_classes = [IsAuthenticated]

    def get(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        rooms = team.voice_rooms.select_related('created_by').all()
        return Response(VoiceRoomSerializer(rooms, many=True).data)

    def post(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        _require_admin(team, request.user)
        serializer = VoiceRoomSerializer(data=request.data)
        serializer.is_valid(raise_exception=True)
        serializer.save(team=team, created_by=request.user)
        return Response(serializer.data, status=status.HTTP_201_CREATED)


class TeamVoiceRoomDetailView(APIView):
    permission_classes = [IsAuthenticated]

    def _get_room(self, team_pk, room_pk, user):
        team = _get_team_or_403(team_pk, user)
        try:
            return team.voice_rooms.select_related('created_by').get(pk=room_pk)
        except VoiceRoom.DoesNotExist:
            from rest_framework.exceptions import NotFound
            raise NotFound('Voice room not found.')

    def get(self, request, pk, room_pk):
        room = self._get_room(pk, room_pk, request.user)
        return Response(VoiceRoomSerializer(room).data)

    def delete(self, request, pk, room_pk):
        team = _get_team_or_403(pk, request.user)
        _require_admin(team, request.user)
        room = self._get_room(pk, room_pk, request.user)
        room.delete()
        return Response(status=status.HTTP_204_NO_CONTENT)


class TeamEditingSessionsView(APIView):
    """Start collaborative code-editing sessions and review past reports.

    No detail/listing view for "rooms" themselves is exposed - clients
    connect to each other directly using the session's `room_key`. The
    only thing kept here is the historical record (reports) for
    demonstration/analytics purposes.
    """

    permission_classes = [IsAuthenticated]

    def get(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        reports = (
            EditingReport.objects
            .filter(session__team=team)
            .select_related('session', 'session__created_by')
        )
        return Response(EditingReportSerializer(reports, many=True).data)

    def post(self, request, pk):
        team = _get_team_or_403(pk, request.user)
        session = EditingSession.objects.create(team=team, created_by=request.user)
        return Response(EditingSessionSerializer(session).data, status=status.HTTP_201_CREATED)


class TeamEditingSessionReportView(APIView):
    permission_classes = [IsAuthenticated]

    def post(self, request, pk, session_id):
        team = _get_team_or_403(pk, request.user)
        try:
            session = team.editing_sessions.get(pk=session_id)
        except EditingSession.DoesNotExist:
            raise NotFound('Editing session not found.')
        if hasattr(session, 'report'):
            return Response(
                {'detail': 'Report already submitted for this session.'},
                status=status.HTTP_400_BAD_REQUEST,
            )
        serializer = EditingReportSerializer(data=request.data)
        serializer.is_valid(raise_exception=True)
        report = serializer.save(session=session)
        session.ended_at = timezone.now()
        session.save(update_fields=['ended_at'])
        return Response(EditingReportSerializer(report).data, status=status.HTTP_201_CREATED)
