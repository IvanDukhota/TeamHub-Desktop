from django.contrib import admin

from .models import Team, TeamMember, TeamRole, VoiceRoom


class TeamMemberInline(admin.TabularInline):
    model = TeamMember
    extra = 0
    raw_id_fields = ['user', 'role']


class TeamRoleInline(admin.TabularInline):
    model = TeamRole
    extra = 0


@admin.register(Team)
class TeamAdmin(admin.ModelAdmin):
    list_display = ['name', 'created_by', 'created_at']
    search_fields = ['name']
    raw_id_fields = ['created_by']
    inlines = [TeamRoleInline, TeamMemberInline]


@admin.register(TeamRole)
class TeamRoleAdmin(admin.ModelAdmin):
    list_display = ['name', 'team', 'is_admin']
    list_filter = ['is_admin']


@admin.register(TeamMember)
class TeamMemberAdmin(admin.ModelAdmin):
    list_display = ['user', 'team', 'role', 'joined_at']
    raw_id_fields = ['user', 'team', 'role']


@admin.register(VoiceRoom)
class VoiceRoomAdmin(admin.ModelAdmin):
    list_display = ['name', 'team', 'max_participants', 'created_by', 'created_at']
    list_filter = ['team']
    search_fields = ['name']
    raw_id_fields = ['created_by']
