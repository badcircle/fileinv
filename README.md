# File Scanner with SQLite Database

A fast C program that scans directories recursively and stores file information in a SQLite database. It tracks file changes over time and can identify deleted files.

## Features

- Fast recursive directory scanning
- SQLite database storage with WAL journaling mode
- Tracks file metadata:
  - Name and full path
  - File extension (lowercase)
  - File size
  - Creation, modification, and access times
  - Windows file attributes
  - Directory status
- Detects file changes and deletions with `-refresh` mode
- Uses prepared statements for efficient database operations
- Indexes for faster querying

## Requirements

- MinGW-w64 (for Windows)
- SQLite3 development libraries
- C compiler (GCC recommended)

## Building

With MinGW-w64:
```bash
gcc -o file_scanner file_scanner.c -lsqlite3
```

## Usage

First run (creates database):
```bash
./file_scanner
```

Update existing database and mark deleted files:
```bash
./file_scanner -refresh
```

## Database Schema

The program creates a SQLite database named `file_inventory.db` with the following schema:

```sql
CREATE TABLE files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    path TEXT NOT NULL,
    extension TEXT,
    size INTEGER,
    is_directory INTEGER,
    created_time INTEGER,
    modified_time INTEGER,
    accessed_time INTEGER,
    attributes TEXT,
    is_deleted INTEGER DEFAULT 0,
    last_seen INTEGER
);
```

## Example Queries

View all current files:
```sql
SELECT 
    name,
    extension,
    size,
    datetime(created_time, 'unixepoch', 'localtime') as created,
    datetime(modified_time, 'unixepoch', 'localtime') as modified,
    attributes
FROM files
WHERE is_deleted = 0;
```

Group files by extension:
```sql
SELECT 
    extension,
    COUNT(*) as count,
    SUM(size) as total_size
FROM files 
WHERE is_deleted = 0 AND is_directory = 0
GROUP BY extension
ORDER BY count DESC;
```

Find recently modified files:
```sql
SELECT 
    name,
    path,
    datetime(modified_time, 'unixepoch', 'localtime') as modified
FROM files
WHERE is_deleted = 0 
AND modified_time > strftime('%s', 'now', '-1 day')
ORDER BY modified_time DESC;
```

## File Attributes

The `attributes` field contains one or more of the following Windows file attributes:
- R: Read-only
- H: Hidden
- S: System
- D: Directory
- A: Archive
- N: Normal

## Notes

- The program skips the database file itself during scanning
- Times are stored as Unix timestamps (seconds since 1970-01-01)
- File paths use Windows-style backslashes
- Extensions are stored in lowercase
- The database uses WAL mode for better performance

## License

[Add your chosen license here]