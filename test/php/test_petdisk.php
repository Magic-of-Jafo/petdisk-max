<?php
// Tests for www/petdisk.php. Sends the same requests the PETdisk MAX firmware
// sends, plus hostile ones, against a running server.
//
//   php -S 127.0.0.1:8080 -t <root> &   (root contains petdisk.php)
//   php test_petdisk.php http://127.0.0.1:8080/petdisk.php <root>
//
// See run.sh, which does all of this in Docker for several PHP versions.

$url = $argv[1];
$root = rtrim($argv[2], '/');
$failures = 0;
$count = 0;

function check($name, $ok, $detail = '')
{
    global $failures, $count;
    $count++;
    if ($ok) {
        echo "ok   $name\n";
    } else {
        $failures++;
        echo "FAIL $name $detail\n";
    }
}

function req($method, $query, $body = '')
{
    global $url;
    $ctx = stream_context_create(array('http' => array(
        'method' => $method,
        'content' => $body,
        'ignore_errors' => true,
        'protocol_version' => 1.0,
        'header' => "Content-Length: ".strlen($body)."\r\n",
    )));
    $resp = @file_get_contents($url.$query, false, $ctx);
    $status = 0;
    if (isset($http_response_header[0]) && preg_match('/ (\d{3})/', $http_response_header[0], $m)) {
        $status = (int)$m[1];
    }
    return array($status, $resp === false ? '' : $resp);
}

function get($query) { $r = req('GET', $query); return $r[1]; }
function put($query, $data) { return req('PUT', $query.'&b64=1', base64_encode($data)); }

// wait for the server to come up
for ($i = 0; $i < 100 && req('GET', '?file=TIME&l=1')[0] != 200; $i++) {
    usleep(100000);
}

// --- fixture -----------------------------------------------------------------
@mkdir("$root/GAMES");
@mkdir("$root/games2");
@mkdir("$root/.hidden");
file_put_contents("$root/a.prg", "\x01\x04HELLO");
file_put_contents("$root/B.SEQ", "SEQDATA");
file_put_contents("$root/notes.txt", "not a pet file");
file_put_contents("$root/.secret.prg", "hidden");
file_put_contents("$root/GAMES/STARTREK.PRG", "\x01\x04TREK");
file_put_contents("$root/.hidden/x.prg", "x");
file_put_contents(dirname($root)."/outside.prg", "OUTSIDE");
@symlink(dirname($root), "$root/escape");
@symlink(dirname($root)."/outside.prg", "$root/link.prg");

// --- directory listings (old firmware: ?d=1&p=N) -------------------------------
check('root listing, files only', get('?d=1&p=0') === "A.PRG\nB.SEQ\n\n", var_export(get('?d=1&p=0'), true));
check('listing without page', get('?d=1') === "A.PRG\nB.SEQ\n\n");
check('page past the end is empty', get('?d=1&p=5') === "\n");
check('listing with subfolders', get('?d=1&p=0&sub=1') === "GAMES/\nGAMES2/\nA.PRG\nB.SEQ\n\n", var_export(get('?d=1&p=0&sub=1'), true));
check('subfolder listing', get('?d=1&p=0&dir=GAMES') === "STARTREK.PRG\n\n");
check('subfolder listing ignores case', get('?d=1&p=0&dir=games') === "STARTREK.PRG\n\n");
check('listing outside root refused', get('?d=1&p=0&dir=..') === "\n");
check('listing hidden folder refused', get('?d=1&p=0&dir=.hidden') === "\n");
check('listing through symlink outside refused', get('?d=1&p=0&dir=escape') === "\n");
check('listing missing folder is empty', get('?d=1&p=0&dir=NOPE') === "\n");
check('listing missing folder is 404', req('GET', '?d=1&p=0&dir=NOPE')[0] === 404);
check('listing existing folder is 200', req('GET', '?d=1&p=0&dir=GAMES')[0] === 200);

// --- sizes and reads -----------------------------------------------------------
check('size', get('?file=A.PRG&l=1') === "7\r\n");
check('size ignores case', get('?file=a.prg&l=1') === "7\r\n");
check('missing file has size 0', get('?file=NOPE.PRG&l=1') === "0\r\n");
check('range read', get('?file=A.PRG&s=0&e=3') === "\x01\x04H");
check('range past end is short', get('?file=A.PRG&s=2&e=513') === "HELLO");
check('whole file', get('?file=B.SEQ') === "SEQDATA");
check('file in subfolder', get('?file=GAMES%2FSTARTREK.PRG&s=0&e=513') === "\x01\x04TREK");
check('size in subfolder', get('?file=games/startrek.prg&l=1') === "6\r\n");
check('time', preg_match('/^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\n$/', get('?file=TIME&s=0&e=20')) === 1);
check('time size', get('?file=TIME&l=1') === "20\r\n");

// --- hostile reads -------------------------------------------------------------
check('parent path refused', get('?file=..%2Foutside.prg&l=1') === "0\r\n");
check('parent path read refused', get('?file=..%2Foutside.prg') === "");
check('nested parent path refused', get('?file=GAMES%2F..%2F..%2Foutside.prg') === "");
check('absolute path refused', get('?file=%2Fetc%2Fpasswd&l=1') === "0\r\n");
check('non-PET file refused', get('?file=notes.txt') === "" && get('?file=notes.txt&l=1') === "0\r\n");
check('script source refused', get('?file=petdisk.php') === "");
check('hidden file refused', get('?file=.secret.prg') === "");
check('symlink to outside file refused', get('?file=link.prg') === "");
check('symlinked folder refused', get('?file=escape%2Foutside.prg') === "");
check('null byte refused', get('?file=A.PRG%00.txt&l=1') === "0\r\n");
check('oversized range refused', req('GET', '?file=A.PRG&s=0&e=100000')[0] === 400);

// --- writes (old firmware: ?f=NAME&n=1 then ?f=NAME, base64) --------------------
put('?f=NEW.PRG&n=1', "\x01\x04ABC");
put('?f=NEW.PRG', "DEF");
check('new file then append', file_get_contents("$root/NEW.PRG") === "\x01\x04ABCDEF");
put('?f=NEW.PRG&n=1', "X");
check('n=1 replaces file', file_get_contents("$root/NEW.PRG") === "X");
put('?f=a.prg&n=1', "LOWER");
check('save matches existing name ignoring case', file_get_contents("$root/a.prg") === "LOWER" && !file_exists("$root/A.PRG.tmp"));
put('?f=GAMES%2FSAVED.SEQ&n=1', "IN FOLDER");
check('save into subfolder', file_get_contents("$root/GAMES/SAVED.SEQ") === "IN FOLDER");

file_put_contents("$root/DISK.D64", str_repeat("\0", 1024));
put('?f=DISK.D64&u=1&s=256&e=260', "ABCD");
$d64 = file_get_contents("$root/DISK.D64");
check('block update', substr($d64, 256, 4) === "ABCD" && strlen($d64) === 1024);

// --- hostile writes ------------------------------------------------------------
$r = put('?f=evil.php&n=1', '<?php echo "pwned"; ?>');
check('writing a script refused', $r[0] === 403 && !file_exists("$root/evil.php"));
$r = put('?f=..%2Fevil.prg&n=1', 'x');
check('writing outside root refused', $r[0] === 403 && !file_exists(dirname($root)."/evil.prg"));
$r = put('?f=GAMES%2F..%2F..%2Fevil.prg&n=1', 'x');
check('nested escape refused', $r[0] === 403 && !file_exists(dirname($root)."/evil.prg"));
$r = put('?f=escape%2Fevil.prg&n=1', 'x');
check('write through symlinked folder refused', $r[0] === 403 && !file_exists(dirname($root)."/evil.prg"));
$r = put('?f=link.prg&n=1', 'x');
check('write through symlinked file refused', $r[0] === 403 && file_get_contents(dirname($root)."/outside.prg") === "OUTSIDE");
$r = put('?f=NOPE%2FX.PRG&n=1', 'x');
check('write into missing folder refused', $r[0] === 403);
$r = put('?f=notes.txt&n=1', 'x');
check('overwriting non-PET file refused', $r[0] === 403 && file_get_contents("$root/notes.txt") === "not a pet file");
$r = put('?f=DISK.D64&u=1&s=0&e=99999', 'x');
check('oversized block update refused', $r[0] === 400);
$r = req('PUT', '?f=BAD.PRG&n=1&b64=1', '***not base64***');
check('bad base64 refused', $r[0] === 400 && !file_exists("$root/BAD.PRG"));

// --- paging --------------------------------------------------------------------
@mkdir("$root/MANY");
for ($i = 0; $i < 80; $i++) {
    file_put_contents(sprintf("$root/MANY/PROGRAM-NUMBER-%03d.PRG", $i), "x");
}
$seen = array();
$pagesOk = true;
for ($p = 0; $p < 20; $p++) {
    $page = get("?d=1&p=$p&dir=MANY");
    if (strlen($page) > 512 || substr($page, -1) !== "\n") { $pagesOk = false; }
    if ($page === "\n") { break; }
    foreach (explode("\n", trim($page)) as $n) { $seen[] = $n; }
}
check('pages fit in 512 bytes', $pagesOk);
check('pages cover every file once', count($seen) === 80 && count(array_unique($seen)) === 80, count($seen));

echo "\n$count checks, $failures failed\n";
exit($failures ? 1 : 0);
