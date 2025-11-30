import 'dart:async';
import 'package:geolocator/geolocator.dart';

import 'package:rover_app/models/track_point.dart';
import 'package:rover_app/repo/trip_repository.dart';

class TripRecorder {
  final TripRepository repo;

  StreamSubscription<Position>? _sub;
  String? _activeTripId;
  DateTime? _lastSavedTs;
  Position? _lastSavedPos;

 
  final int minSeconds = 2;          // was 2
  final double minMeters = 3.0;      // was 5.0
  final double maxHdopMeters = 150.0; // was 50.0

  TripRecorder(this.repo);

  bool get isRecording => _activeTripId != null;

  Future<bool> _ensurePermissions() async {
    //print('[TripRecorder] Checking location permission...');
    LocationPermission p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) {
      //print('[TripRecorder] Permission denied, requesting...');
      p = await Geolocator.requestPermission();
    }

    if (p == LocationPermission.deniedForever || p == LocationPermission.denied) {
     // print('[TripRecorder] Permission denied (forever or now).');
      return false;
    }

    final enabled = await Geolocator.isLocationServiceEnabled();
    //print('[TripRecorder] Location services enabled = $enabled, permission = $p');
    return enabled;
  }

  Future<String?> start(String name) async {
   // print('[TripRecorder] start("$name") called');
    

    if (!await _ensurePermissions()) {
      //print('[TripRecorder] Aborting start(): permissions or location disabled.');
      return null;
    }

    final trip = await repo.createTrip(name);
    _activeTripId = trip.id;
    _lastSavedTs = null;
    _lastSavedPos = null;

    //print('[TripRecorder] Created trip id=${trip.id}');

    final stream = Geolocator.getPositionStream(
      locationSettings: const LocationSettings(
        accuracy: LocationAccuracy.best,
        distanceFilter: 0,
      ),
    );

    _sub = stream.listen((pos) async {
      /*print('[TripRecorder] Raw position: '
          'lat=${pos.latitude}, lon=${pos.longitude}, '
          'acc=${pos.accuracy}, speed=${pos.speed}');
*/
      if (pos.accuracy.isNaN || pos.accuracy > maxHdopMeters) {
       // print('[TripRecorder] Skipping point: bad accuracy (${pos.accuracy} m)');
        return;
      }

      final now = DateTime.now().toUtc();

      final timeOk = _lastSavedTs == null ||
          now.difference(_lastSavedTs!).inSeconds >= minSeconds;

      final dist = _lastSavedPos == null
          ? 0.0
          : Geolocator.distanceBetween(
              _lastSavedPos!.latitude,
              _lastSavedPos!.longitude,
              pos.latitude,
              pos.longitude,
            );

          final distOk = _lastSavedPos == null ||
          Geolocator.distanceBetween(
                _lastSavedPos!.latitude,
                _lastSavedPos!.longitude,
                pos.latitude,
                pos.longitude,
              ) >= minMeters;

      //print('[TripRecorder] timeOk=$timeOk, distOk=$distOk, dist=$dist m, '
        //  'isRecording=$isRecording');

      if (timeOk && distOk && _activeTripId != null) {
        final tp = TrackPoint(
          tripId: _activeTripId!,
          tsUtc: now,
          lat: pos.latitude,
          lon: pos.longitude,
          alt: pos.altitude,
          speed: pos.speed,
          headingDeg: pos.heading,
        );

        //print('[TripRecorder] Saving TrackPoint for trip=$_activeTripId '
           // 'lat=${tp.lat}, lon=${tp.lon}');

        await repo.insertPoint(tp);
        _lastSavedTs = now;
        _lastSavedPos = pos;
      }
    }); 

    return _activeTripId;
  }

  Future<void> stop() async {
    //print('[TripRecorder] stop() called');

    await _sub?.cancel();
    _sub = null;

    final id = _activeTripId;
    _activeTripId = null;

    if (id != null) {
      //print('[TripRecorder] Finalizing trip $id');
      await repo.finalizeTrip(id, simplifyToleranceM: 5);
    } 
  }
}















/*
import 'dart:async';
import 'package:geolocator/geolocator.dart';

import 'package:rover_app/models/track_point.dart';
import 'package:rover_app/repo/trip_repository.dart';

class TripRecorder {
  final TripRepository repo;

  StreamSubscription<Position>? _sub;
  String? _activeTripId;
  DateTime? _lastSavedTs;
  Position? _lastSavedPos;

  // Tuning knobs
  final int minSeconds = 2;     // time throttle
  final double minMeters = 5.0; // distance throttle
  final double maxHdopMeters = 50.0; // use 'accuracy' as a proxy

  TripRecorder(this.repo);

  bool get isRecording => _activeTripId != null;

  Future<bool> _ensurePermissions() async {
    LocationPermission p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) {
      p = await Geolocator.requestPermission();
    }
    if (p == LocationPermission.deniedForever || p == LocationPermission.denied) {
      return false;
    }
    final enabled = await Geolocator.isLocationServiceEnabled();
    return enabled;
  }

  Future<String?> start(String name) async {
    if (!await _ensurePermissions()) return null;

    final trip = await repo.createTrip(name);
    _activeTripId = trip.id;
    _lastSavedTs = null;
    _lastSavedPos = null;

    final stream = Geolocator.getPositionStream(
      locationSettings: const LocationSettings(
        accuracy: LocationAccuracy.best,
        distanceFilter: 0,
      ),

      
    );

    _sub = stream.listen((pos) async {
      if (pos.accuracy.isNaN || pos.accuracy > maxHdopMeters) return;

      final now = DateTime.now().toUtc();

      final timeOk = _lastSavedTs == null ||
          now.difference(_lastSavedTs!).inSeconds >= minSeconds;

      final distOk = _lastSavedPos == null ||
          Geolocator.distanceBetween(
                  _lastSavedPos!.latitude,
                  _lastSavedPos!.longitude,
                  pos.latitude,
                  pos.longitude) >= minMeters;

      if (timeOk && distOk && _activeTripId != null) {
        final tp = TrackPoint(
          tripId: _activeTripId!,
          tsUtc: now,
          lat: pos.latitude,
          lon: pos.longitude,
          alt: pos.altitude,
          speed: pos.speed,
          headingDeg: pos.heading,
        );
        await repo.insertPoint(tp);
        _lastSavedTs = now;
        _lastSavedPos = pos;
      }
    });

    return _activeTripId;
  }

  Future<void> stop() async {
    await _sub?.cancel();
    _sub = null;
    final id = _activeTripId;
    _activeTripId = null;
    if (id != null) {
      await repo.finalizeTrip(id, simplifyToleranceM: 5);
    }
  }
}
*/
