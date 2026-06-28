import uuid

from django.conf import settings
from django.db import models


class Team(models.Model):
    id = models.UUIDField(primary_key=True, default=uuid.uuid4, editable=False)
    name = models.CharField(max_length=100)
    description = models.TextField(blank=True)
    created_by = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        on_delete=models.CASCADE,
        related_name='created_teams',
    )
    created_at = models.DateTimeField(auto_now_add=True)
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        ordering = ['-created_at']

    def __str__(self):
        return self.name


class TeamRole(models.Model):
    id = models.UUIDField(primary_key=True, default=uuid.uuid4, editable=False)
    team = models.ForeignKey(Team, on_delete=models.CASCADE, related_name='roles')
    name = models.CharField(max_length=50)
    is_admin = models.BooleanField(default=False)
    can_view = models.BooleanField(default=True)
    can_create_projects = models.BooleanField(default=False)
    can_edit_team = models.BooleanField(default=False)
    can_manage_settings = models.BooleanField(default=False)
    can_delete = models.BooleanField(default=False)

    class Meta:
        unique_together = ('team', 'name')

    def __str__(self):
        return f'{self.name} ({self.team.name})'


class TeamMember(models.Model):
    id = models.UUIDField(primary_key=True, default=uuid.uuid4, editable=False)
    team = models.ForeignKey(Team, on_delete=models.CASCADE, related_name='members')
    user = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        on_delete=models.CASCADE,
        related_name='team_memberships',
    )
    role = models.ForeignKey(
        TeamRole,
        on_delete=models.SET_NULL,
        null=True,
        blank=True,
        related_name='members',
    )
    joined_at = models.DateTimeField(auto_now_add=True)

    class Meta:
        unique_together = ('team', 'user')
        ordering = ['joined_at']

    def __str__(self):
        return f'{self.user.email} in {self.team.name}'


class EditingSession(models.Model):
    """A temporary collaborative code-editing session.

    The session itself carries no live state - the actual real-time
    connection between participants happens out-of-band (e.g. a CRDT/
    WebSocket service) using `id` as the shared room key. Django only
    records that the session happened and, once it ends, the report
    describing what was done in it.
    """

    id = models.UUIDField(primary_key=True, default=uuid.uuid4, editable=False)
    team = models.ForeignKey(Team, on_delete=models.CASCADE, related_name='editing_sessions')
    created_by = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        on_delete=models.SET_NULL,
        null=True,
        related_name='created_editing_sessions',
    )
    started_at = models.DateTimeField(auto_now_add=True)
    ended_at = models.DateTimeField(null=True, blank=True)

    class Meta:
        ordering = ['-started_at']

    def __str__(self):
        return f'EditingSession {self.id} ({self.team.name})'


class EditingReport(models.Model):
    id = models.UUIDField(primary_key=True, default=uuid.uuid4, editable=False)
    session = models.OneToOneField(EditingSession, on_delete=models.CASCADE, related_name='report')
    summary = models.TextField(blank=True)
    changed_files = models.JSONField(default=list, blank=True)
    participants = models.JSONField(default=list, blank=True)
    created_at = models.DateTimeField(auto_now_add=True)

    class Meta:
        ordering = ['-created_at']

    def __str__(self):
        return f'Report for session {self.session_id}'


class VoiceRoom(models.Model):
    id = models.UUIDField(primary_key=True, default=uuid.uuid4, editable=False)
    team = models.ForeignKey(Team, on_delete=models.CASCADE, related_name='voice_rooms')
    name = models.CharField(max_length=100)
    created_by = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        on_delete=models.SET_NULL,
        null=True,
        related_name='created_voice_rooms',
    )
    max_participants = models.PositiveSmallIntegerField(default=0, help_text='0 = unlimited')
    created_at = models.DateTimeField(auto_now_add=True)
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        ordering = ['name']
        unique_together = ('team', 'name')

    def __str__(self):
        return f'{self.name} ({self.team.name})'
