import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart'; // Using Riverpod for reactivity
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';
import 'package:path_provider/path_provider.dart';

import 'package:rover_app/app_services.dart'; 
import 'package:rover_app/models/trip.dart';
import 'package:rover_app/models/track_point.dart';
import 'package:rover_app/utils/geo.dart';

// --- Riverpod Providers (Assumed setup) ---
// 1. Provides the stream of ALL trips (sorted by date)
final allTripsStreamProvider = StreamProvider.autoDispose<List<Trip>>((ref) {
  return AppServices.I.repo.getTripsStream();
});

// 2. State for the currently selected trip (managed by the dropdown)
final selectedTripIdProvider = StateProvider<String?>((ref) => null);

// 3. Provides the TrackPoints for the selected trip
final selectedTripPointsProvider = FutureProvider.autoDispose<List<TrackPoint>>((ref) async {
  final tripId = ref.watch(selectedTripIdProvider);
  if (tripId == null) return [];
  return AppServices.I.repo.getPointsForTrip(tripId);
});

// 4. State for the recording status (read from AppServices)
final isRecordingProvider = StateProvider<bool>((ref) => AppServices.I.recorder.isRecording);
// -------------------------------------------


Future<String?> _askTripNameDialog(BuildContext context) async {
  final controller = TextEditingController();
  return showDialog<String>(
    context: context,
    builder: (ctx) {
      return AlertDialog(
        title: const Text('Name your trip'),
        content: TextField(
          controller: controller,
          decoration: const InputDecoration(
            labelText: 'Trip name',
            hintText: 'e.g. Airport run, Walk to class',
          ),
          autofocus: true,
        ),
        actions: [
          TextButton(
            onPressed: () {
              Navigator.of(ctx).pop(null); // user canceled
            },
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () {
              Navigator.of(ctx).pop(controller.text.trim());
            },
            child: const Text('Start'),
          ),
        ],
      );
    },
  );
}


class TripLoggerPage extends ConsumerWidget {
  const TripLoggerPage({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // Watch all trips for the dropdown and list
    final tripsAsync = ref.watch(allTripsStreamProvider);
    final selectedTripId = ref.watch(selectedTripIdProvider);
    final isRecording = ref.watch(isRecordingProvider);

    // Find the currently selected trip object
    final selectedTrip = tripsAsync.maybeWhen(
      data: (trips) {
        try {
          return trips.firstWhere((t) => t.id == selectedTripId);
        } catch (_) {
          return null;
        }
      },
      orElse: () => null,
    );

    // Watch the points for the selected trip
    final pointsAsync = ref.watch(selectedTripPointsProvider);

    return Scaffold(
      appBar: AppBar(
        title: const Text('Trip Logger'),
        actions: [
          IconButton(
            onPressed: selectedTrip == null ? null : () => _exportGpx(context, selectedTrip, ref),
            icon: const Icon(Icons.ios_share),
            tooltip: 'Export GPX',
          ),
        ],
      ),
      body: Column(
        children: [
          // Trip selector
          Padding(
            padding: const EdgeInsets.all(8.0),
            child: tripsAsync.when(
              loading: () => const LinearProgressIndicator(),
              error: (e, s) => Text('Error loading trips: $e'),
              data: (trips) {
                final ids = trips.map((t) => t.id).toSet().toList();
                // If selected ID is null or no longer exists, try to select the most recent one
                if (selectedTripId == null && ids.isNotEmpty) {
                  WidgetsBinding.instance.addPostFrameCallback((_) {
                    ref.read(selectedTripIdProvider.notifier).state = ids.first;
                  });
                }
                final safeValue = ids.contains(selectedTripId) ? selectedTripId : null;
                
                return DropdownButton<String>(
                  key: ValueKey('trip-dd-${AppServices.I.currentUid ?? "nouser"}'),
                  isExpanded: true,
                  value: safeValue,
                  hint: const Text('Select a Trip'),
                  items: trips.map((t) {
                    final subtitle = t.isActive
                        ? 'ACTIVE • ${t.pointCount} pts'
                        : '${(t.distanceMeters / 1000).toStringAsFixed(2)} km • ${t.pointCount} pts';
                    return DropdownMenuItem(
                      value: t.id,
                      child: Text('${t.name} — $subtitle'),
                    );
                  }).toList(),
                  onChanged: (id) {
                    ref.read(selectedTripIdProvider.notifier).state = id;
                  },
                );
              },
            ),
          ),

          // Map
          Expanded(
            child: pointsAsync.when(
              loading: () => const Center(child: CircularProgressIndicator()),
              error: (e, s) => Center(child: Text('Error loading points: $e')),
              data: (points) {
                final polyPoints = points.map((p) => LatLng(p.lat, p.lon)).toList(growable: false);
                final center = polyPoints.isNotEmpty
                    ? polyPoints.last
                    : const LatLng(30.6185, -96.3365); // Default map center (Texas A&M)

                return FlutterMap(
                  options: MapOptions(
                    initialCenter: center,
                    initialZoom: 16,
                    interactionOptions: const InteractionOptions(
                      enableMultiFingerGestureRace: true,
                    ),
                    // Tap to manually refresh points (optional, but harmless)
                    onTap: (_, __) => ref.invalidate(selectedTripPointsProvider), 
                  ),
                  children: [
                    TileLayer(
                      urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                      userAgentPackageName: 'com.example.rover_app',
                    ),
                    PolylineLayer(
                      polylines: [
                        Polyline(
                          points: polyPoints,
                          strokeWidth: 4,
                          color: selectedTrip?.isActive == true ? Colors.blue.shade700 : Colors.purple.shade700,
                        ),
                      ],
                    ),
                    if (polyPoints.isNotEmpty)
                      MarkerLayer(
                        markers: [
                          Marker(
                            point: polyPoints.last,
                            width: 30,
                            height: 30,
                            child: Icon(Icons.location_pin, size: 30, color: selectedTrip?.isActive == true ? Colors.red : Colors.green),
                          ),
                        ],
                      ),
                  ],
                );
              },
            ),
          ),

          // Controls
          SafeArea(
            top: false,
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
              child: Row(
                children: [
                  Expanded(
                    child: ElevatedButton.icon(
                      onPressed: isRecording ? null : () => _startRecording(context, ref),
                      icon: const Icon(Icons.play_arrow),
                      label: const Text('Start'),
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: ElevatedButton.icon(
                      onPressed: isRecording ? () => _stopRecording(context, ref) : null,
                      icon: const Icon(Icons.stop),
                      label: const Text('Stop'),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  // --- Helper Methods ---

Future<void> _startRecording(BuildContext context, WidgetRef ref) async {
  // 1) Ask user for a custom name
  final inputName = await _askTripNameDialog(context);

  // If user cancelled the dialog, do nothing
  if (inputName == null) {
    return;
  }

  // 2) Fallback default name if they left it blank
  final name = inputName.isEmpty
      ? "Trip ${DateTime.now().toLocal().toString().substring(0, 19)}"
      : inputName;

  // 3) Call recorder with that name
  final id = await AppServices.I.recorder.start(name);

  if (id == null) {
    if (!context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text("Location permission/service required.")),
    );
    return;
  }

  // 4) Mark as recording in state
  ref.read(isRecordingProvider.notifier).state = true;

  // 5) Select the new active trip
  ref.read(selectedTripIdProvider.notifier).state = id;

}



/*
  Future<void> _startRecording(BuildContext context, WidgetRef ref) async {
    // 1. Refresh recording status
    ref.read(isRecordingProvider.notifier).state = true;
    
    final name = "Trip ${DateTime.now().toLocal().toString().substring(0, 19)}";
    
    // 2. Call recorder start method
    final id = await AppServices.I.recorder.start(name);

    if (id == null) {
      if (!context.mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text("Location permission/service required.")),
      );
      ref.read(isRecordingProvider.notifier).state = false; // Reset status on failure
      return;
    }

    // 3. Select the new active trip
    ref.read(selectedTripIdProvider.notifier).state = id;
  }
  */

  Future<void> _stopRecording(BuildContext context, WidgetRef ref) async {
    await AppServices.I.recorder.stop();
    
    // Invalidate providers to force a refresh of the recording status and points
    ref.read(isRecordingProvider.notifier).state = false;
    ref.invalidate(allTripsStreamProvider);
    ref.invalidate(selectedTripPointsProvider);
  }

  Future<void> _exportGpx(BuildContext context, Trip trip, WidgetRef ref) async {
    final points = await ref.read(selectedTripPointsProvider.future);
    if (points.isEmpty) {
      if (!context.mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text("Cannot export empty trip.")),
      );
      return;
    }

    final gpx = toGpx(trip.name, points);
    final dir = await getApplicationDocumentsDirectory();
    final file = File('${dir.path}/${trip.id}.gpx');
    await file.writeAsString(gpx);
    
    if (!context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text("Exported: ${file.path}")),
    );
  }
}
