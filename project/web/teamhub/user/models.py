from django.contrib.auth.models import AbstractBaseUser, PermissionsMixin
from django.db import models

from .managers import UserManager


def user_avatar_path(instance, filename):
    return f'avatars/{instance.pk}/{filename}'


class User(AbstractBaseUser, PermissionsMixin):
    GENDER_CHOICES = [
        ('male', 'Male'),
        ('female', 'Female'),
    ]

    REGION_CHOICES = [
        ('north_america', 'North America'),
        ('south_america', 'South America'),
        ('europe', 'Europe'),
        ('asia', 'Asia'),
        ('africa', 'Africa'),
        ('oceania', 'Oceania'),
        ('middle_east', 'Middle East'),
    ]

    LANGUAGE_CHOICES = [
        ('en', 'English'),
        ('uk', 'Ukrainian'),
        ('ru', 'Russian'),
        ('de', 'German'),
        ('fr', 'French'),
        ('es', 'Spanish'),
        ('pl', 'Polish'),
    ]

    email = models.EmailField(unique=True)
    username = models.CharField(max_length=50, unique=True)
    avatar = models.ImageField(upload_to=user_avatar_path, blank=True, null=True)

    language = models.CharField(max_length=10, default='en', choices=LANGUAGE_CHOICES)
    gender = models.CharField(max_length=20, blank=True, default='', choices=GENDER_CHOICES)
    age = models.PositiveSmallIntegerField(null=True, blank=True)
    region = models.CharField(max_length=50, blank=True, default='', choices=REGION_CHOICES)

    is_active = models.BooleanField(default=True)
    is_staff = models.BooleanField(default=False)

    created_at = models.DateTimeField(auto_now_add=True)
    updated_at = models.DateTimeField(auto_now=True)

    objects = UserManager()

    USERNAME_FIELD = 'email'
    REQUIRED_FIELDS = ['username']

    class Meta:
        ordering = ['username', 'email']

    def __str__(self):
        return self.email

    @property
    def full_name(self):
        return self.username

    @property
    def avatar_url(self):
        if self.avatar:
            return self.avatar.url
        return None
