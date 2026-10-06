<?php
// PETdisk MAX network drive server.
//
// Serves PET files (.PRG, .SEQ, .D64) from the folder this script lives in
// (or $PETDISK_ROOT) to a PETdisk MAX over HTTP.
//
// Protocol (unchanged from earlier versions, so older firmware keeps working):
//   GET  ?file=NAME&l=1          file size followed by "\r\n" ("0" if missing)
//   GET  ?file=NAME&s=S&e=E      bytes S..E-1 of a file (whole file without e)
//   GET  ?file=TIME              current UTC time "YYYY-MM-DD HH:mm:ss\n"
//   GET  ?d=1[&p=N]              directory listing, one name per line,
//                                page ends with a blank line
//   PUT  ?f=NAME&n=1             start a new file with this block
//   PUT  ?f=NAME                 append a block
//   PUT  ?f=NAME&u=1&s=S&e=E     overwrite bytes S..E-1 (D64 block writes)
//   PUT  ...&b64=1               request body is base64 encoded
//
// Additions (optional, ignored by older firmware):
//   NAME may include subfolders: GAMES/STARTREK.PRG
//   GET  ?d=1&dir=GAMES          list a subfolder
//   GET  ?d=1&sub=1              also list subfolders, as "NAME/"
//
// Every path is confined to $PETDISK_ROOT: "..", hidden names and other file
// types are rejected, so the script can't be used to read or write other
// files on the server.

// Set for read only mode
$PETDISK_READ_ONLY = false;

// Folder holding the PET files. Defaults to this script's folder.
$PETDISK_ROOT = __DIR__;

// File types the PET can read and write.
$PETDISK_EXTENSIONS = array('prg', 'seq', 'd64');

// Largest file a PET may create (a D64 image is 174848 bytes, a D81 819200).
$PETDISK_MAX_FILE_SIZE = 1024 * 1024;

// Largest byte range served or written in one request.
$PETDISK_MAX_RANGE = 4096;

define('DIR_PAGE_SIZE', 512);

function getParam($paramname)
{
    if (isset($_GET[$paramname]) && is_string($_GET[$paramname]))
    {
        return $_GET[$paramname];
    }

    return false;
}

function intParam($paramname)
{
    $value = getParam($paramname);
    if ($value === false || !ctype_digit($value))
    {
        return false;
    }
    return (int)$value;
}

function respond($body)
{
    header('Content-Length: '.strlen($body));
    header('Content-Type: application/octet-stream');
    header('Connection: close');
    echo $body;
    flush();
}

function rootDir()
{
    global $PETDISK_ROOT;
    return rtrim(str_replace('\\', '/', realpath($PETDISK_ROOT)), '/');
}

// Split a requested path into safe components, or return false.
function pathComponents($rel)
{
    if ($rel === false || strpos($rel, "\0") !== false)
    {
        return false;
    }

    $parts = array();
    foreach (explode('/', str_replace('\\', '/', $rel)) as $part)
    {
        if ($part === '' || $part === '.')
        {
            continue;
        }
        // no parent references, hidden files or drive letters
        if ($part === '..' || $part[0] === '.' || strpos($part, ':') !== false)
        {
            return false;
        }
        $parts[] = $part;
    }
    return $parts;
}

// Find an entry in $dir by name, preferring an exact match, then ignoring case.
function findEntry($dir, $name)
{
    if (file_exists($dir.'/'.$name))
    {
        return $name;
    }

    $entries = @scandir($dir);
    if ($entries === false)
    {
        return false;
    }
    $lower = strtolower($name);
    foreach ($entries as $entry)
    {
        if (strtolower($entry) === $lower)
        {
            return $entry;
        }
    }
    return false;
}

// True if $path (which must exist) is inside the root, following symlinks.
function insideRoot($path)
{
    $root = rootDir();
    $real = realpath($path);
    if ($real === false)
    {
        return false;
    }
    $real = str_replace('\\', '/', $real);
    return $real === $root || strncmp($real, $root.'/', strlen($root) + 1) === 0;
}

// Resolve a folder below the root. Returns its path or false.
function resolveDir($rel)
{
    $parts = ($rel === false) ? array() : pathComponents($rel);
    if ($parts === false)
    {
        return false;
    }

    $dir = rootDir();
    foreach ($parts as $part)
    {
        $entry = findEntry($dir, $part);
        if ($entry === false || !is_dir($dir.'/'.$entry))
        {
            return false;
        }
        $dir = $dir.'/'.$entry;
    }
    return insideRoot($dir) ? $dir : false;
}

// Resolve a PET file below the root. With $create, a missing file resolves to
// the path it would be created at. Returns the path or false.
function resolveFile($rel, $create)
{
    global $PETDISK_EXTENSIONS;

    $parts = pathComponents($rel);
    if ($parts === false || count($parts) == 0)
    {
        return false;
    }

    $name = array_pop($parts);
    $ext = strtolower(pathinfo($name, PATHINFO_EXTENSION));
    if (!in_array($ext, $PETDISK_EXTENSIONS, true))
    {
        return false;
    }

    $dir = resolveDir(implode('/', $parts));
    if ($dir === false)
    {
        return false;
    }

    $entry = findEntry($dir, $name);
    if ($entry !== false)
    {
        $path = $dir.'/'.$entry;
        return (is_file($path) && insideRoot($path)) ? $path : false;
    }

    return $create ? $dir.'/'.$name : false;
}

function timeString()
{
    $currentDate = new DateTime();
    $currentDate->setTimezone(new DateTimeZone("UTC"));
    return $currentDate->format("Y-m-d H:i:s\n");
}

function directoryListing($dir, $page, $withSubdirs)
{
    global $PETDISK_EXTENSIONS;

    $names = array();
    $entries = ($dir === false) ? false : @scandir($dir);
    if ($entries !== false)
    {
        $dirs = array();
        $files = array();
        foreach ($entries as $entry)
        {
            if ($entry === '' || $entry[0] === '.')
            {
                continue;
            }
            $path = $dir.'/'.$entry;
            if (!insideRoot($path))
            {
                continue;
            }
            if (is_dir($path))
            {
                if ($withSubdirs)
                {
                    $dirs[] = strtoupper($entry).'/';
                }
            }
            else if (in_array(strtolower(pathinfo($entry, PATHINFO_EXTENSION)), $PETDISK_EXTENSIONS, true))
            {
                $files[] = strtoupper($entry);
            }
        }
        sort($dirs, SORT_STRING);
        sort($files, SORT_STRING);
        $names = array_merge($dirs, $files);
    }

    // Without a page number, send everything in one response (old behaviour).
    if ($page === false)
    {
        return implode('', array_map(function ($n) { return $n."\n"; }, $names))."\n";
    }

    // Each page, including its terminating blank line, fits in DIR_PAGE_SIZE.
    $pages = array();
    $current = '';
    foreach ($names as $name)
    {
        $entry = $name."\n";
        if (strlen($current) + strlen($entry) >= DIR_PAGE_SIZE)
        {
            $pages[] = $current;
            $current = '';
        }
        $current .= $entry;
    }
    $pages[] = $current;

    return ($page < count($pages)) ? $pages[$page]."\n" : "\n";
}

header_remove("X-Powered-By");

$verb = $_SERVER['REQUEST_METHOD'];
if ($verb == "GET")
{
    $fileParam = getParam('file');

    if (getParam('d') == 1 && $fileParam === false)
    {
        $dir = resolveDir(getParam('dir'));
        respond(directoryListing($dir, intParam('p'), getParam('sub') == 1));
    }
    else if ($fileParam === "TIME")
    {
        if (getParam('l') == 1)
        {
            respond(strlen(timeString())."\r\n");
        }
        else
        {
            respond(timeString());
        }
    }
    else if ($fileParam !== false)
    {
        $file = resolveFile($fileParam, false);

        if (getParam('l') == 1)
        {
            // a missing file reports size 0, which the PETdisk treats as not found
            respond(($file === false ? 0 : filesize($file))."\r\n");
        }
        else if ($file === false)
        {
            error_log("petdisk: file not found: ".$fileParam);
            http_response_code(404);
            respond("");
        }
        else
        {
            $start = intParam('s');
            $end = intParam('e');
            if ($end !== false && $end > 0)
            {
                if ($start === false || $end <= $start || $end - $start > $PETDISK_MAX_RANGE)
                {
                    http_response_code(400);
                    respond("");
                    return;
                }
                $contents = '';
                $fp = fopen($file, "rb");
                if ($fp !== false)
                {
                    fseek($fp, $start, SEEK_SET);
                    $contents = (string)fread($fp, $end - $start);
                    fclose($fp);
                }
                respond($contents);
            }
            else
            {
                respond(file_get_contents($file));
            }
        }
    }
}
else if ($verb == "PUT")
{
    if ($PETDISK_READ_ONLY == true)
    {
        // ignore writes for read only mode
        return;
    }

    $file = resolveFile(getParam('f'), true);
    if ($file === false)
    {
        error_log("petdisk: refused write to ".getParam('f'));
        http_response_code(403);
        respond("");
        return;
    }

    // read put data
    $putdata = file_get_contents("php://input");
    if (getParam('b64'))
    {
        $putdata = base64_decode($putdata, true);
        if ($putdata === false)
        {
            http_response_code(400);
            respond("");
            return;
        }
    }

    if (getParam('u') == 1) // update specific block
    {
        $start = intParam('s');
        $end = intParam('e');
        if ($start === false || $end === false || $end <= $start ||
            $end - $start > $PETDISK_MAX_RANGE || $end > $PETDISK_MAX_FILE_SIZE || !is_file($file))
        {
            error_log("petdisk: bad block update for ".getParam('f'));
            http_response_code(400);
            respond("");
            return;
        }
        $fp = fopen($file, "r+b");
        if ($fp !== false && flock($fp, LOCK_EX))
        {
            fseek($fp, $start);
            fwrite($fp, substr($putdata, 0, $end - $start));
            flock($fp, LOCK_UN);
        }
        if ($fp !== false)
        {
            fclose($fp);
        }
    }
    else // first block of a new file, or append block to end of file
    {
        $mode = (getParam('n') == 1) ? "wb" : "ab";
        $existing = ($mode == "ab" && is_file($file)) ? filesize($file) : 0;
        if ($existing + strlen($putdata) > $PETDISK_MAX_FILE_SIZE)
        {
            error_log("petdisk: file too large: ".getParam('f'));
            http_response_code(413);
            respond("");
            return;
        }
        $fp = fopen($file, $mode);
        if ($fp !== false && flock($fp, LOCK_EX))
        {
            fwrite($fp, $putdata);
            flock($fp, LOCK_UN);
        }
        if ($fp !== false)
        {
            fclose($fp);
        }
    }
    respond("");
}
