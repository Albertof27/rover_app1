class UserProfile {
  final String uid;
  final String name;
  final String email;
  final int? age;

  UserProfile({
    required this.uid,
    required this.name,
    required this.email,
    this.age,
  });

  factory UserProfile.fromMap(String uid, Map<String, dynamic> data) {
    return UserProfile(
      uid: uid,
      name: data['name'] ?? '',
      email: data['email'] ?? '',
      age: (data['age'] as num?)?.toInt(),
    );
  }

  Map<String, dynamic> toMap() {
    return {
      'name': name,
      'email': email,
      'age': age,
    };
  }
}