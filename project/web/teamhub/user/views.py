from rest_framework import generics, status
from rest_framework.authtoken.models import Token
from rest_framework.permissions import AllowAny, IsAuthenticated
from rest_framework.response import Response
from rest_framework.views import APIView

from .models import User
from .serializers import LoginSerializer, RegisterSerializer, UserProfileSerializer


class RegisterView(generics.CreateAPIView):
    queryset = User.objects.all()
    serializer_class = RegisterSerializer
    permission_classes = [AllowAny]

    def create(self, request, *args, **kwargs):
        serializer = self.get_serializer(data=request.data)
        serializer.is_valid(raise_exception=True)
        user = serializer.save()
        token, _ = Token.objects.get_or_create(user=user)
        return Response(
            {'token': token.key, 'user': UserProfileSerializer(user).data},
            status=status.HTTP_201_CREATED,
        )


class LoginView(APIView):
    permission_classes = [AllowAny]

    def post(self, request):
        serializer = LoginSerializer(data=request.data)
        serializer.is_valid(raise_exception=True)
        user = serializer.validated_data['user']
        token, _ = Token.objects.get_or_create(user=user)
        return Response({'token': token.key, 'user': UserProfileSerializer(user).data})


class LogoutView(APIView):
    permission_classes = [IsAuthenticated]

    def post(self, request):
        request.user.auth_token.delete()
        return Response(status=status.HTTP_204_NO_CONTENT)


class ProfileView(generics.RetrieveUpdateAPIView):
    serializer_class = UserProfileSerializer
    permission_classes = [IsAuthenticated]

    def get_object(self):
        return self.request.user


class MyTeamsView(APIView):
    permission_classes = [IsAuthenticated]

    def get(self, request):
        from team.serializers import TeamSerializer
        memberships = request.user.team_memberships.select_related('team__created_by').all()
        teams = [m.team for m in memberships]
        serializer = TeamSerializer(teams, many=True, context={'request': request})
        return Response(serializer.data)


class MyRoomsView(APIView):
    permission_classes = [IsAuthenticated]

    def get(self, request):
        from team.models import VoiceRoom
        from team.serializers import VoiceRoomSerializer
        team_ids = request.user.team_memberships.values_list('team_id', flat=True)
        rooms = (
            VoiceRoom.objects
            .filter(team_id__in=team_ids)
            .select_related('team', 'created_by')
            .order_by('team__name', 'name')
        )
        return Response(VoiceRoomSerializer(rooms, many=True).data)
