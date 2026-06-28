from django.contrib.auth import get_user_model
from rest_framework import serializers

from user.serializers import UserBriefSerializer
from .models import EditingReport, EditingSession, Team, TeamMember, TeamRole, VoiceRoom

User = get_user_model()


class TeamRoleSerializer(serializers.ModelSerializer):
    class Meta:
        model = TeamRole
        fields = [
            'id', 'name', 'is_admin',
            'can_view', 'can_create_projects', 'can_edit_team',
            'can_manage_settings', 'can_delete',
        ]
        read_only_fields = ['id']


class TeamMemberSerializer(serializers.ModelSerializer):
    user = UserBriefSerializer(read_only=True)
    role = TeamRoleSerializer(read_only=True)
    user_id = serializers.PrimaryKeyRelatedField(
        source='user',
        queryset=User.objects.all(),
        write_only=True,
    )
    role_id = serializers.PrimaryKeyRelatedField(
        source='role',
        queryset=TeamRole.objects.all(),
        write_only=True,
        required=False,
        allow_null=True,
    )

    class Meta:
        model = TeamMember
        fields = ['id', 'user', 'user_id', 'role', 'role_id', 'joined_at']
        read_only_fields = ['id', 'joined_at']


class TeamSerializer(serializers.ModelSerializer):
    created_by = UserBriefSerializer(read_only=True)
    members_count = serializers.SerializerMethodField()

    class Meta:
        model = Team
        fields = ['id', 'name', 'description', 'created_by', 'members_count', 'created_at', 'updated_at']
        read_only_fields = ['id', 'created_by', 'created_at', 'updated_at']

    def get_members_count(self, obj):
        return obj.members.count()

    def create(self, validated_data):
        user = self.context['request'].user
        team = Team.objects.create(created_by=user, **validated_data)
        TeamMember.objects.create(team=team, user=user)
        return team


class TeamDetailSerializer(TeamSerializer):
    members = TeamMemberSerializer(many=True, read_only=True)
    roles = TeamRoleSerializer(many=True, read_only=True)

    class Meta(TeamSerializer.Meta):
        fields = TeamSerializer.Meta.fields + ['members', 'roles']


class EditingSessionSerializer(serializers.ModelSerializer):
    created_by = UserBriefSerializer(read_only=True)
    room_key = serializers.UUIDField(source='id', read_only=True)

    class Meta:
        model = EditingSession
        fields = ['id', 'room_key', 'created_by', 'started_at', 'ended_at']
        read_only_fields = fields


class EditingReportSerializer(serializers.ModelSerializer):
    session_id = serializers.UUIDField(source='session.id', read_only=True)
    started_by = UserBriefSerializer(source='session.created_by', read_only=True)
    started_at = serializers.DateTimeField(source='session.started_at', read_only=True)
    ended_at = serializers.DateTimeField(source='session.ended_at', read_only=True)

    class Meta:
        model = EditingReport
        fields = [
            'id', 'session_id', 'started_by', 'started_at', 'ended_at',
            'summary', 'changed_files', 'participants', 'created_at',
        ]
        read_only_fields = ['id', 'session_id', 'started_by', 'started_at', 'ended_at', 'created_at']


class VoiceRoomSerializer(serializers.ModelSerializer):
    created_by = UserBriefSerializer(read_only=True)
    team_id = serializers.UUIDField(source='team.id', read_only=True)
    team_name = serializers.CharField(source='team.name', read_only=True)
    # UUID that the signaling server uses as room key
    room_key = serializers.UUIDField(source='id', read_only=True)

    class Meta:
        model = VoiceRoom
        fields = [
            'id', 'room_key', 'team_id', 'team_name',
            'name', 'max_participants', 'created_by',
            'created_at', 'updated_at',
        ]
        read_only_fields = ['id', 'room_key', 'team_id', 'team_name', 'created_by', 'created_at', 'updated_at']
