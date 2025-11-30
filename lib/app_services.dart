import 'package:hive_flutter/hive_flutter.dart';
import 'package:hive/hive.dart';
import 'package:rover_app/models/trip.dart';
import 'package:rover_app/models/track_point.dart';
import 'package:rover_app/repo/trip_repository.dart';
import 'package:rover_app/services/trip_recorder.dart';
import 'package:rover_app/repo/user_profile_repository.dart';

class AppServices {
  AppServices._();
  static final AppServices I = AppServices._();

  TripRepository? _repo;
  TripRecorder? _recorder;
  String? _currentUid;
  UserProfileRepository? _userProfileRepo;

  TripRepository get repo => _repo!;
  TripRecorder get recorder => _recorder!;
  String? get currentUid => _currentUid;

  UserProfileRepository get userProfileRepo => _userProfileRepo!;

  static Future<void> initHiveOnce() async {
    await Hive.initFlutter();

    if (!Hive.isAdapterRegistered(1)) {
      Hive.registerAdapter(TripAdapter());
    }
    if (!Hive.isAdapterRegistered(2)) {
       Hive.registerAdapter(TrackPointAdapter());
    }
  }

  /// Initialize services for a specific user.
  Future<void> initForUser(String uid) async {
    if (_currentUid == uid && _repo != null && _recorder != null && _userProfileRepo != null) return;

    // Close previous user's boxes if switching users
    await _repo?.close();

    final repo = await TripRepository.initForUser(uid);
    final recorder = TripRecorder(repo);

    final profileRepo = UserProfileRepository(uid: uid);

    _repo = repo;
    _recorder = recorder;
    _userProfileRepo = profileRepo;
    _currentUid = uid;
  }

  /// Clear references when user signs out.
  Future<void> clearForSignOut() async {
    await _repo?.close();
    _repo = null;
    _recorder = null;
    _userProfileRepo = null;
    _currentUid = null;
  }
}

