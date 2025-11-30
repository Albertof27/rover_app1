import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:firebase_core/firebase_core.dart';
import 'state/auth_providers.dart';
import 'ui/login_screen.dart';
import 'ui/home.dart';
import 'firebase_options.dart';
import 'app_services.dart';

Future<void> main() async {
  // Ensures Flutter widgets are ready before async initialization
  WidgetsFlutterBinding.ensureInitialized();

  // Initialize Firebase (uses google-services.json on Android)
  await Firebase.initializeApp(
    options: DefaultFirebaseOptions.currentPlatform,
  );
  // Initialize Hive, TripRepository, and TripRecorder before app launch
  await AppServices.initHiveOnce();

  // Start your app wrapped in Riverpod ProviderScope
  runApp(const ProviderScope(child: MyApp()));
}

class MyApp extends ConsumerWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // Watch Firebase auth state (you’ll define authUserProvider in auth_providers.dart)
    final auth = ref.watch(authUserProvider);

    auth.whenData((user) {
      WidgetsBinding.instance.addPostFrameCallback((_) async {
        if (user != null) {
          await AppServices.I.initForUser(user.uid);
          ref.invalidate(tripsStreamProvider);
          ref.invalidate(totalDistanceKmProvider);

        } else {
          await AppServices.I.clearForSignOut();
          ref.invalidate(tripsStreamProvider);
          ref.invalidate(totalDistanceKmProvider);
        }
      });
    });


    return MaterialApp(
      title: 'Rover App',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      // Display login screen or home depending on auth state
      home: auth.when(
        data: (user) => user == null ? const LoginScreen() : const RoverHome(),
        loading: () => const Scaffold(
          body: Center(child: CircularProgressIndicator()),
        ),
        error: (err, _) => Scaffold(
          body: Center(child: Text('Auth error: ${err.toString()}')),
        ),
      ),
    );
  }
}
