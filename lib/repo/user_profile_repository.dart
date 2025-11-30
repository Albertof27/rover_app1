import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import '../models/user_profile.dart';

class UserProfileRepository {
  final String uid;

  UserProfileRepository({required this.uid});

  final _firestore = FirebaseFirestore.instance;
  final _auth = FirebaseAuth.instance;

  // Strongly typed users collection
  CollectionReference<Map<String, dynamic>> get _usersCol =>
      _firestore.collection('users');

  // ----------------- PROFILE (name, email, age) -----------------

  Future<UserProfile> getProfile() async {
    final doc = await _usersCol.doc(uid).get();
    final data = doc.data();

    if (!doc.exists || data == null) {
      // First time: create a default profile from auth info
      final user = _auth.currentUser;

      final profile = UserProfile(
        uid: uid,
        name: user?.displayName ?? '',
        email: user?.email ?? '',
        age: null,
      );

      await _usersCol.doc(uid).set(profile.toMap(), SetOptions(merge: true));
      return profile;
    }

    return UserProfile.fromMap(uid, data);
  }

  Future<void> saveProfile(UserProfile profile) async {
    await _usersCol
        .doc(profile.uid)
        .set(profile.toMap(), SetOptions(merge: true));
  }

  Stream<UserProfile?> profileStream() {
    return _usersCol.doc(uid).snapshots().map((doc) {
      final data = doc.data();
      if (!doc.exists || data == null) return null;
      return UserProfile.fromMap(uid, data);
    });
  }

  // ----------------- LUGGAGE CHECKLIST -----------------

  Future<List<Map<String, dynamic>>> getLuggageChecklist() async {
    final doc = await _usersCol.doc(uid).get();
    final data = doc.data();

    if (data == null || data['luggageChecklist'] == null) {
      return [];
    }

    final rawList = data['luggageChecklist'] as List<dynamic>;

    return rawList
        .map((e) => Map<String, dynamic>.from(e as Map))
        .toList();
  }

  Future<void> saveLuggageChecklist(List<Map<String, dynamic>> items) async {
    await _usersCol.doc(uid).set(
      {
        'luggageChecklist': items,
      },
      SetOptions(merge: true),
    );
  }



  Future<List<List<String>>> getLuggageChecklistHistory() async {
  final historyCol =
      _usersCol.doc(uid).collection('luggageChecklistHistory');

  final snapshots = await historyCol
      .orderBy('createdAt', descending: true)
      .get();

  return snapshots.docs.map((doc) {
    final data = doc.data();
    final rawItems = data['items'] as List<dynamic>? ?? [];
    return rawItems.map((e) => e.toString()).toList();
  }).toList();
} 

/*
  Future<List<List<String>>> getLuggageChecklistHistory() async {
    final doc = await _usersCol.doc(uid).get();
    final data = doc.data();

    if (data == null || data['luggageChecklistHistory'] == null) {
      return [];
    }

    final rawList = data['luggageChecklistHistory'] as List<dynamic>;

    return rawList
        .map(
          (e) => (e as List<dynamic>).map((s) => s.toString()).toList(),
        )
        .toList();
  }
*/

  Future<void> addChecklistToHistory(List<String> itemTexts) async {
  if (itemTexts.isEmpty) return;

  final historyCol =
      _usersCol.doc(uid).collection('luggageChecklistHistory');

  await historyCol.add({
    'items': itemTexts,
    'createdAt': FieldValue.serverTimestamp(),
  });
}

/*
  Future<void> addChecklistToHistory(List<String> itemTexts) async {
    if (itemTexts.isEmpty) return;

    await _usersCol.doc(uid).set(
      {
        'luggageChecklistHistory': FieldValue.arrayUnion([itemTexts]),
      },
      SetOptions(merge: true),
    );
  }

  */
}



/*
import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import '../models/user_profile.dart';

class UserProfileRepository {
  final String uid;
  UserProfileRepository({required this.uid});
  final _firestore = FirebaseFirestore.instance;
  final _auth = FirebaseAuth.instance;

  CollectionReference get _usersCol => _firestore.collection('users');
/*
  String get _uid {
    final user = _auth.currentUser;
    if (user == null) throw Exception('No user logged in');
    return user.uid;
  }
  */

  Future<UserProfile> getProfile() async {
   // final uid = _uid;
    final doc = await _usersCol.doc(uid).get();

    if (!doc.exists) {
      // Create a default profile from auth info
      final user = _auth.currentUser!;
      final profile = UserProfile(
        uid: uid,
        name: user.displayName ?? '',
        email: user.email ?? '',
        age: null,
      );
      await _usersCol.doc(uid).set(profile.toMap());
      return profile;
    }
    
    final data = doc.data()!;

    return UserProfile.fromMap(uid, doc.data() as Map<String, dynamic>);
  }

  Future<void> saveProfile(UserProfile profile) async {
    await _usersCol.doc(profile.uid).set(profile.toMap(), SetOptions(merge: true));
  }

  Stream<UserProfile?> profileStream() {
   
    return _usersCol.doc(uid).snapshots().map((doc) {
      if (!doc.exists || doc.data() == null) return null;
      return UserProfile.fromMap(uid, doc.data() as Map<String, dynamic>);
    });
  }
}
*/
