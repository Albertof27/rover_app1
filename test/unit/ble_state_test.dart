import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'package:rover_app/state/ble_state.dart';  

void main() {
  group('Weight / overload providers', () {
    test('isOverloadedProvider is true when weight > threshold', () {
      final container = ProviderContainer();
      addTearDown(container.dispose);

      container.read(weightThresholdProvider.notifier).state = 20.0;
      container.read(weightProvider.notifier).state = 25.0;

      final overloaded = container.read(isOverloadedProvider);
      expect(overloaded, isTrue);
    });

    test('isOverloadedProvider is false when weight <= threshold', () {
      final container = ProviderContainer();
      addTearDown(container.dispose);

      container.read(weightThresholdProvider.notifier).state = 20.0;
      container.read(weightProvider.notifier).state = 19.9;

      final overloaded = container.read(isOverloadedProvider);
      expect(overloaded, isFalse);
    });

    test('weightStringProvider formats weight to 1 decimal place', () {
      final container = ProviderContainer();
      addTearDown(container.dispose);

      container.read(weightProvider.notifier).state = 18.345;
      final text = container.read(weightStringProvider);

      expect(text, '18.3 lbs');
    });
  });

  group('RSSI → distance + out-of-range providers', () {
    test('rssiToDistanceMeters returns ~1 m at txPowerAt1m', () {
      const cfg = RssiDistanceConfig(txPowerAt1m: -59, pathLossExponent: 2.0);
      final d = rssiToDistanceMeters(-59, cfg);
      expect(d, moreOrLessEquals(1.0, epsilon: 0.05));
    });

    test('distanceMetersProvider uses rssiProvider and config', () {
      final container = ProviderContainer();
      addTearDown(container.dispose);

      final cfg = container.read(rssiDistanceConfigProvider);

      container.read(rssiProvider.notifier).state = cfg.txPowerAt1m.toInt();

      final d = container.read(distanceMetersProvider);
      expect(d, isNotNull);
      expect(d!, moreOrLessEquals(1.0, epsilon: 0.05));
    });

    test('outOfRangeProvider is false when distance < 1.83 m', () {
      final container = ProviderContainer();
      addTearDown(container.dispose);

      final cfg = container.read(rssiDistanceConfigProvider);
      container.read(rssiProvider.notifier).state = cfg.txPowerAt1m.toInt();

      final outOfRange = container.read(outOfRangeProvider);
      expect(outOfRange, isFalse);
    });

    test('outOfRangeProvider is true when distance > 1.83 m', () {
      final container = ProviderContainer();
      addTearDown(container.dispose);

      container.read(rssiProvider.notifier).state = -75;

      final d = container.read(distanceMetersProvider);
      expect(d, isNotNull);
      expect(d!, greaterThan(1.83));

      final outOfRange = container.read(outOfRangeProvider);
      expect(outOfRange, isTrue);
    });
  });
}





