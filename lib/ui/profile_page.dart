import 'package:flutter/material.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:rover_app/app_services.dart';
import '../models/user_profile.dart';

class ProfilePage extends StatefulWidget {
  const ProfilePage({super.key});

  @override
  State<ProfilePage> createState() => _ProfilePageState();
}

class _LuggageItem {
  String text;
  bool checked;

  _LuggageItem({required this.text, this.checked = false});
}

class _ProfilePageState extends State<ProfilePage> {
  final _formKey = GlobalKey<FormState>();
  final _nameController = TextEditingController();
  final _ageController = TextEditingController();

  final _newItemController = TextEditingController();
  List<_LuggageItem> _checklist = [];
  List<List<String>> _checklistHistory = [];

  bool _loading = true;
  bool _isEditing = false;
  bool _isNewProfile = false;
  UserProfile? _profile;

  @override
  void initState() {
    super.initState();
    _loadProfile();
  }

  @override
  void dispose() {
    _nameController.dispose();
    _ageController.dispose();
    _newItemController.dispose();
    super.dispose();
  }



  Future<void> _loadProfile() async {
  try {
    final repo = AppServices.I.userProfileRepo;

    // Load main profile
    final profile = await repo.getProfile();

    // Load luggage checklist + history
    final rawChecklist = await repo.getLuggageChecklist();
    final history = await repo.getLuggageChecklistHistory();

    // "New" if name empty and no age
    final isNew = profile.name.trim().isEmpty && profile.age == null;

    setState(() {
      _profile = profile;
      _nameController.text = profile.name;
      _ageController.text =
          profile.age != null ? profile.age.toString() : '';
      _isNewProfile = isNew;
      _isEditing = isNew;
      _loading = false;

      _checklist = rawChecklist
          .map(
            (m) => _LuggageItem(
              text: m['text'] as String? ?? '',
              checked: m['checked'] as bool? ?? false,
            ),
          )
          .toList();

      _checklistHistory = history;
    });
  } catch (e) {
    setState(() {
      _loading = false;
    });
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Failed to load profile: $e')),
      );
    }
  }
}

/*
  Future<void> _loadProfile() async {
    try {
      final repo = AppServices.I.userProfileRepo;
      final profile = await repo.getProfile();

      // "New" if name empty and default maxLoadLbs
      final isNew = profile.name.trim().isEmpty && profile.age == null;

      setState(() {
        _profile = profile;
        _nameController.text = profile.name;
        _ageController.text = profile.age != null ? profile.age.toString() : '';
        _isNewProfile = isNew;
        _isEditing = isNew; // auto-enter edit mode if no info yet
        _loading = false;
      });
    } catch (e) {
      setState(() {
        _loading = false;
      });
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to load profile: $e')),
        );
      }
    }
  }
*/
 
 




void _addChecklistItem() {
  final text = _newItemController.text.trim();
  if (text.isEmpty) return;

  setState(() {
    _checklist.add(_LuggageItem(text: text));
    _newItemController.clear();
  });
}

Future<void> _saveChecklist() async {
  if (_profile == null) return;

  final repo = AppServices.I.userProfileRepo;

  // Convert to Firestore-friendly maps
  final payload = _checklist
      .map((item) => {
            'text': item.text,
            'checked': item.checked,
          })
      .toList();

  try {
    await repo.saveLuggageChecklist(payload);
    await repo.addChecklistToHistory(
      _checklist.map((e) => e.text).toList(),
    );

    setState(() {
      _checklistHistory.add(
        _checklist.map((e) => e.text).toList(),
      );
    });

    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Luggage checklist saved')),
    );
  } catch (e) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Failed to save checklist: $e')),
    );
  }
}

void _removeChecklistItem(int index) {
  setState(() {
    _checklist.removeAt(index);
  });
}


void _showChecklistHistoryPicker() {
  if (_checklistHistory.isEmpty) {
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('No previous checklists yet')),
    );
    return;
  }

  showModalBottomSheet(
    context: context,
    builder: (ctx) {
      return SafeArea(
        child: ListView.builder(
          itemCount: _checklistHistory.length,
          itemBuilder: (ctx, index) {
            final items = _checklistHistory[index];
            return ListTile(
              title: Text('Checklist ${index + 1}'),
              subtitle: Text(
                items.join(', '),
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
              ),
              onTap: () {
                Navigator.of(ctx).pop();
                setState(() {
                  _checklist = items
                      .map((text) => _LuggageItem(text: text))
                      .toList();
                });
              },
            );
          },
        ),
      );
    },
  );
}


 
 
 
  Future<void> _saveProfile() async {
    if (!_formKey.currentState!.validate() || _profile == null) return;
    final parsedAge = int.tryParse(_ageController.text.trim());

    final updated = UserProfile(
      uid: _profile!.uid,
      name: _nameController.text.trim(),
      email: _profile!.email, // keep same email
      age: parsedAge,
    );

    await AppServices.I.userProfileRepo.saveProfile(updated);

    setState(() {
      _profile = updated;
      _isEditing = false;
      _isNewProfile = false;
    });

    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Profile saved')),
      );
    }
  }

  void _toggleEdit() {
    setState(() {
      if (_isEditing && _profile != null) {
        // cancel: restore last saved values
        _nameController.text = _profile!.name;
        _ageController.text = _profile!.age != null ? _profile!.age.toString() : '';
      }
      _isEditing = !_isEditing;
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return Scaffold(
        appBar: AppBar(title: const Text('Profile')),
        body: const Center(child: CircularProgressIndicator()),
      );
    }

    final firebaseUser = FirebaseAuth.instance.currentUser;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Profile'),
        actions: [
          IconButton(
            icon: Icon(_isEditing ? Icons.close : Icons.edit),
            tooltip: _isEditing ? 'Cancel' : 'Edit',
            onPressed: _toggleEdit,
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Form(
          key: _formKey,
          child: ListView(
            children: [
              if (firebaseUser?.photoURL != null)
                CircleAvatar(
                  radius: 40,
                  backgroundImage: NetworkImage(firebaseUser!.photoURL!),
                ),
              const SizedBox(height: 16),

              Text(
                'Email: ${_profile?.email ?? firebaseUser?.email ?? ''}',
                style: Theme.of(context).textTheme.bodyMedium,
              ),
              const SizedBox(height: 16),

              if (_isNewProfile && !_isEditing)
                Text(
                  'No profile info yet. Tap Edit to add your details.',
                  style: Theme.of(context)
                      .textTheme
                      .bodySmall
                      ?.copyWith(color: Colors.grey[700]),
                ),
              const SizedBox(height: 16),

              TextFormField(
                controller: _nameController,
                enabled: _isEditing,
                decoration: const InputDecoration(
                  labelText: 'Name',
                  border: OutlineInputBorder(),
                ),
                validator: (v) {
                  if (!_isEditing) return null; // only validate when editing
                  if (v == null || v.trim().isEmpty) {
                    return 'Please enter your name';
                  }
                  return null;
                },
              ),
              const SizedBox(height: 24),

              TextFormField(
                controller: _ageController,
                enabled: _isEditing,
                decoration: const InputDecoration(
                  labelText: 'Age',
                  border: OutlineInputBorder(),
                ),
                keyboardType: TextInputType.number,
                validator: (v) {
                  if (!_isEditing) return null;
                  final value = double.tryParse(v ?? '');
                  if (value == null || value <= 0 || value > 120) {
                    return 'Enter a valid age';
                  }
                  return null;
                },
              ),
              const SizedBox(height: 24),

              
              if (_isEditing)
                ElevatedButton(
                  onPressed: _saveProfile,
                  child: const Text('Save'),
                )
              else
                Text(
                  'Profile is read-only. Tap Edit to make changes.',
                  style: Theme.of(context)
                      .textTheme
                      .bodySmall
                      ?.copyWith(color: Colors.grey[700]),
                ),

              Center(
                child: Image.asset(
                  'assets/images/Gemini_Generated_Image_oopwucoopwucoopw.png',
                  width: 300,
                  height: 300,
                  fit: BoxFit.contain,
                ),
              ),
              
              const SizedBox(height: 24),


              // ---------------- LUGGAGE CHECK LIST ----------------

// ---------------- LUGGAGE CHECK LIST ----------------
Text(
  'Luggage Check List',
  style: Theme.of(context).textTheme.titleMedium,
),
const SizedBox(height: 8),

Row(
  children: [
    Expanded(
      child: TextField(
        controller: _newItemController,
        decoration: const InputDecoration(
          labelText: 'Add item',
          border: OutlineInputBorder(),
        ),
        onSubmitted: (_) => _addChecklistItem(),
      ),
    ),
    const SizedBox(width: 8),
    IconButton(
      icon: const Icon(Icons.add),
      tooltip: 'Add item',
      onPressed: _addChecklistItem,
    ),
  ],
),
const SizedBox(height: 12),

// ---- THIS IS THE IMPORTANT PART ----
if (_checklist.isEmpty) ...[
  Text(
    'No items yet. Add items to your luggage list.',
    style: Theme.of(context)
        .textTheme
        .bodySmall
        ?.copyWith(color: Colors.grey[700]),
  ),
] else ...[
  Column(
    children: _checklist.asMap().entries.map((entry) {
      final idx = entry.key;
      final item = entry.value;

      return ListTile(
        contentPadding: EdgeInsets.zero,
        leading: Checkbox(
          value: item.checked,
          onChanged: (val) {
            setState(() {
              _checklist[idx].checked = val ?? false;
            });
          },
        ),
        title: Text(item.text),
        trailing: IconButton(
          icon: const Icon(Icons.delete_outline),
          tooltip: 'Remove item',
          onPressed: () => _removeChecklistItem(idx),
        ),
      );
    }).toList(),
  ),
],
// ---- END IMPORTANT PART ----

const SizedBox(height: 12),

Row(
  children: [
    Expanded(
      child: ElevatedButton.icon(
        onPressed: _checklist.isEmpty ? null : _saveChecklist,
        icon: const Icon(Icons.save),
        label: const Text('Save Checklist'),
      ),
    ),
    const SizedBox(width: 8),
    Expanded(
      child: OutlinedButton.icon(
        onPressed: _showChecklistHistoryPicker,
        icon: const Icon(Icons.history),
        label: const Text('Load Previous'),
      ),
    ),
  ],
),
const SizedBox(height: 24),

/*
Text(
  'Luggage Check List',
  style: Theme.of(context).textTheme.titleMedium,
),
const SizedBox(height: 8),

Row(
  children: [
    Expanded(
      child: TextField(
        controller: _newItemController,
        decoration: const InputDecoration(
          labelText: 'Add item',
          border: OutlineInputBorder(),
        ),
        onSubmitted: (_) => _addChecklistItem(),
      ),
    ),
    const SizedBox(width: 8),
    IconButton(
      icon: const Icon(Icons.add),
      tooltip: 'Add item',
      onPressed: _addChecklistItem,
    ),
  ],
),
const SizedBox(height: 12),

if (_checklist.isEmpty)
  Text(
    'No items yet. Add items to your luggage list.',
    style: Theme.of(context)
        .textTheme
        .bodySmall
        ?.copyWith(color: Colors.grey[700]),
  )




if (_checklist.isEmpty)
  Text(
    'No items yet. Add items to your luggage list.',
    style: Theme.of(context)
        .textTheme
        .bodySmall
        ?.copyWith(color: Colors.grey[700]),
  )
else
  Column(
    children: _checklist.asMap().entries.map((entry) {
      final idx = entry.key;
      final item = entry.value;

      return ListTile(
        contentPadding: EdgeInsets.zero,
        leading: Checkbox(
          value: item.checked,
          onChanged: (val) {
            setState(() {
              _checklist[idx].checked = val ?? false;
            });
          },
        ),
        title: Text(item.text),
        trailing: IconButton(
          icon: const Icon(Icons.delete_outline),
          tooltip: 'Remove item',
          onPressed: () => _removeChecklistItem(idx),
        ),
      );
    }).toList(),
  ),


/*else
  Column(
    children: _checklist.asMap().entries.map((entry) {
      final idx = entry.key;
      final item = entry.value;
      return Row(
        children: [
          Expanded(
            child: CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              value: item.checked,
              onChanged: (val) {
                setState(() {
                  _checklist[idx].checked = val ?? false;
                });
              },
              title: Text(item.text),
            ),
          ),
          IconButton(
            icon: const Icon(Icons.delete_outline),
            tooltip: 'Remove item',
            onPressed: () => _removeChecklistItem(idx),
          ),
        ],
      );
    }).toList(),
  ),

*/
const SizedBox(height: 12),

Row(
  children: [
    Expanded(
      child: ElevatedButton.icon(
        onPressed: _checklist.isEmpty ? null : _saveChecklist,
        icon: const Icon(Icons.save),
        label: const Text('Save Checklist'),
      ),
    ),
    const SizedBox(width: 8),
    Expanded(
      child: OutlinedButton.icon(
        onPressed: _showChecklistHistoryPicker,
        icon: const Icon(Icons.history),
        label: const Text('Load Previous'),
      ),
    ),
  ],
),
const SizedBox(height: 24),
*/
// -------------- END LUGGAGE CHECK LIST --------------



            ],
          ),
        ),
      ),
    );
  }
}






/*
import 'package:flutter/material.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:rover_app/app_services.dart';
import '../models/user_profile.dart';

class ProfilePage extends StatefulWidget {
  const ProfilePage({super.key});

  @override
  State<ProfilePage> createState() => _ProfilePageState();
}

class _ProfilePageState extends State<ProfilePage> {
  final _formKey = GlobalKey<FormState>();
  final _nameController = TextEditingController();
  final _maxLoadController = TextEditingController();

  bool _loading = true;
  UserProfile? _profile;

  @override
  void initState() {
    super.initState();
    _loadProfile();
  }

  Future<void> _loadProfile() async {
    try {
      final repo = AppServices.I.userProfileRepo;
      final profile = await repo.getProfile();

      setState(() {
        _profile = profile;
        _nameController.text = profile.name;
        _maxLoadController.text = profile.maxLoadLbs.toStringAsFixed(0);
        _loading = false;
      });
    } catch (e) {
      // handle error – show snackbar, etc.
      setState(() {
        _loading = false;
      });
    }
  }

  Future<void> _saveProfile() async {
    if (!_formKey.currentState!.validate() || _profile == null) return;

    final updated = UserProfile(
      uid: _profile!.uid,
      name: _nameController.text.trim(),
      email: _profile!.email, // keep same email
      maxLoadLbs: double.tryParse(_maxLoadController.text) ?? 20,
    );

    await AppServices.I.userProfileRepo.saveProfile(updated);

    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Profile saved')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return Scaffold(
        appBar: AppBar(title: Text('Profile')),
        body: Center(child: CircularProgressIndicator()),
      );
    }

    final firebaseUser = FirebaseAuth.instance.currentUser;

    return Scaffold(
      appBar: AppBar(title: const Text('Profile')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Form(
          key: _formKey,
          child: ListView(
            children: [
              if (firebaseUser?.photoURL != null)
                CircleAvatar(
                  radius: 40,
                  backgroundImage: NetworkImage(firebaseUser!.photoURL!),
                ),
              const SizedBox(height: 16),
              Text('Email: ${_profile?.email ?? firebaseUser?.email ?? ''}'),
              const SizedBox(height: 16),

              TextFormField(
                controller: _nameController,
                decoration: const InputDecoration(
                  labelText: 'Name',
                  border: OutlineInputBorder(),
                ),
                validator: (v) =>
                    (v == null || v.trim().isEmpty) ? 'Please enter your name' : null,
              ),
              const SizedBox(height: 16),

              TextFormField(
                controller: _maxLoadController,
                decoration: const InputDecoration(
                  labelText: 'Max Load (lbs)',
                  border: OutlineInputBorder(),
                ),
                keyboardType: TextInputType.number,
                validator: (v) {
                  final value = double.tryParse(v ?? '');
                  if (value == null || value <= 0) {
                    return 'Enter a positive number';
                  }
                  return null;
                },
              ),
              const SizedBox(height: 24),

              ElevatedButton(
                onPressed: _saveProfile,
                child: const Text('Save'),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
*/
