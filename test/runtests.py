#!/usr/bin/env python3
"""
NTFS3NT read-path test: build a real NTFS image with mkntfs, drop a couple of
files in with ntfscp, then run the user-mode parser (ntfstest, built from the
same ntfssup.c the kernel driver uses) and check it reads boot, $MFT, the
system records, the root directory index and file data back correctly.

Needs: cc, ntfs-3g / ntfsprogs (mkntfs, ntfscp).
"""
import os, subprocess, sys, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "src")

def need(tool):
    if shutil.which(tool) is None:
        print("SKIP: '%s' not found (install ntfs-3g / ntfsprogs)" % tool); sys.exit(0)

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

def main():
    for t in ("cc", "mkntfs", "ntfscp"):
        need(t)
    tmp = tempfile.mkdtemp(prefix="ntfsnt-")
    try:
        img = os.path.join(tmp, "test.img")
        with open(img, "wb") as f:
            f.truncate(64 * 1024 * 1024)
        run(["mkntfs", "-Q", "-F", "-L", "NTFSTEST", img])

        hello = os.path.join(tmp, "hello.txt")
        open(hello, "w").write("hello from ntfsnt test\n")
        run(["ntfscp", img, hello, "hello.txt"])
        readme = os.path.join(tmp, "readme.txt")
        open(readme, "w").write("second file\n")
        run(["ntfscp", img, readme, "readme.txt"])

        exe = os.path.join(tmp, "ntfstest")
        run(["cc", "-O1", "-Wall", "-Wextra", "-I", SRC,
             os.path.join(HERE, "ntfstest.c"), os.path.join(SRC, "ntfssup.c"),
             "-o", exe])
        out = run([exe, img]).stdout
        print(out)

        checks = [
            ("boot parsed",        "cluster 4096"),
            ("$MFT bootstrap",     "$MFT"),
            ("system $UpCase",     "$UpCase"),
            ("root has hello.txt", "hello.txt  (rec"),
            ("root has readme.txt","readme.txt  (rec"),
            ("resident read",      'hello from ntfsnt test'),
            ("non-resident read",  "$UpCase $DATA non-resident, 131072"),
        ]
        bad = [n for n, s in checks if s not in out]
        if bad:
            print("FAILED checks: " + ", ".join(bad)); sys.exit(1)

        # write round-trip: overwrite hello.txt's resident data in place, then
        # verify ntfs-3g (ntfscat) reads the new content and the volume stays clean.
        rec = None
        for line in out.splitlines():
            if "hello.txt  (rec" in line:
                rec = line.split("rec", 1)[1].split(",")[0].strip()
        if rec and shutil.which("ntfscat") and shutil.which("ntfsfix"):
            wexe = os.path.join(tmp, "ntfswrite")
            run(["cc", "-O1", "-Wall", "-I", SRC, os.path.join(HERE, "ntfswrite.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", wexe])
            run([wexe, img, rec, "WRITTEN-BY-NTFS3NT"])
            cat = run(["ntfscat", img, "hello.txt"]).stdout
            fix = run(["ntfsfix", "-n", img]).stdout
            if "WRITTEN-BY-NTFS3NT" not in cat:
                print("FAILED: write round-trip (ntfscat mismatch)"); sys.exit(1)
            if "processed successfully" not in fix:
                print("FAILED: write round-trip (volume not clean)"); sys.exit(1)
            print("write round-trip: OK (ntfs-3g read-back + clean volume)")

        # create / delete: fresh volume, create files, check ntfs-3g sees them,
        # delete one, re-check, and confirm the volume stays clean throughout.
        if shutil.which("ntfsls") and shutil.which("ntfscat") and shutil.which("ntfsfix"):
            cexe = os.path.join(tmp, "ntfscreate")
            run(["cc", "-O1", "-I", SRC, os.path.join(HERE, "ntfscreate.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", cexe])
            cimg = os.path.join(tmp, "create.img")
            with open(cimg, "wb") as f: f.truncate(64 * 1024 * 1024)
            run(["mkntfs", "-Q", "-F", "-L", "CREATE", cimg])
            run([cexe, cimg, "create", "newfile.txt", "created by NTFS3NT"])
            run([cexe, cimg, "create", "apple.txt", "aaa"])
            run([cexe, cimg, "create", "zebra.txt", "zzz"])
            ls = run(["ntfsls", cimg]).stdout
            cat = run(["ntfscat", cimg, "newfile.txt"]).stdout
            for nm in ("newfile.txt", "apple.txt", "zebra.txt"):
                if nm not in ls: print("FAILED: create (%s missing)" % nm); sys.exit(1)
            if "created by NTFS3NT" not in cat: print("FAILED: create (content)"); sys.exit(1)
            run([cexe, cimg, "delete", "newfile.txt"])
            ls2 = run(["ntfsls", cimg]).stdout
            if "newfile.txt" in ls2: print("FAILED: delete (still listed)"); sys.exit(1)
            if "apple.txt" not in ls2 or "zebra.txt" not in ls2:
                print("FAILED: delete (removed wrong entry)"); sys.exit(1)
            fix = run(["ntfsfix", "-n", cimg]).stdout
            if "processed successfully" not in fix:
                print("FAILED: create/delete (volume not clean)"); sys.exit(1)
            print("create/delete: OK (ntfs-3g listing + content + clean volume)")

        # ntfsfmt: format a volume from scratch, verify it is clean and that
        # ntfs-3g can write a file into it and read it back.
        FMT = os.path.join(HERE, "..", "fmt")
        if shutil.which("ntfsfix") and shutil.which("ntfscp") and shutil.which("ntfscat") \
           and os.path.exists(os.path.join(FMT, "ntfsfmt.c")):
            fexe = os.path.join(tmp, "ntfsfmt")
            run(["cc", "-O1", "-I", SRC, os.path.join(FMT, "ntfsfmt.c"), "-o", fexe])
            fimg = os.path.join(tmp, "fmt.img")
            run([fexe, fimg, "64", "FMTTEST"])
            fix = run(["ntfsfix", "-n", fimg]).stdout
            if "processed successfully" not in fix:
                print("FAILED: ntfsfmt (volume not clean)"); sys.exit(1)
            with open(os.path.join(tmp, "h.txt"), "w") as f: f.write("ntfsfmt round-trip OK")
            run(["ntfscp", fimg, os.path.join(tmp, "h.txt"), "h.txt"])
            if "ntfsfmt round-trip OK" not in run(["ntfscat", fimg, "h.txt"]).stdout:
                print("FAILED: ntfsfmt (round-trip)"); sys.exit(1)
            print("ntfsfmt: OK (ntfsfix clean + ntfs-3g round-trip)")

            # write-grow engine: grow a non-resident file on the ntfsfmt volume
            gexe = os.path.join(tmp, "ntfsgrow")
            run(["cc", "-O1", "-I", SRC, os.path.join(HERE, "ntfsgrow.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", gexe])
            with open(os.path.join(tmp, "nr.bin"), "wb") as f:
                f.write(b"ORIG-" + bytes([65 + (i % 26) for i in range(2000)]))
            run(["ntfscp", fimg, os.path.join(tmp, "nr.bin"), "bigf.txt"])
            inode = None
            for line in run(["ntfsls", "-i", fimg]).stdout.splitlines():
                if "bigf" in line: inode = line.split()[0]
            run([gexe, fimg, inode, "10000"])
            got = run(["ntfscat", fimg, "bigf.txt"]).stdout.encode("latin1", "ignore")
            exp_tail = bytes([65 + ((2005 + i) % 26) for i in range(10)])  # start of grown region
            if len(got) < 12000 or not got.startswith(b"ORIG-"):
                print("FAILED: write-grow (size/head)"); sys.exit(1)
            if "processed successfully" not in run(["ntfsfix", "-n", fimg]).stdout:
                print("FAILED: write-grow (volume not clean)"); sys.exit(1)
            print("write-grow: OK (ntfs-3g read-back + clean volume)")

            # resident -> non-resident conversion (small file grown past its record)
            cvexe = os.path.join(tmp, "ntfsconv")
            run(["cc", "-O1", "-I", SRC, os.path.join(HERE, "ntfsconv.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", cvexe])
            cvimg = os.path.join(tmp, "conv.img"); run([fexe, cvimg, "64", "CONV"])
            with open(os.path.join(tmp, "sm.txt"), "w") as f: f.write("SMALL-RESIDENT-FILE-CONTENT")
            run(["ntfscp", cvimg, os.path.join(tmp, "sm.txt"), "sm.txt"])
            ino = None
            for line in run(["ntfsls", "-i", cvimg]).stdout.splitlines():
                if "sm.txt" in line: ino = line.split()[0]
            run([cvexe, cvimg, ino, "5000"])
            cg = run(["ntfscat", cvimg, "sm.txt"]).stdout.encode("latin1", "ignore")
            if len(cg) != 5027 or not cg.startswith(b"SMALL-RESIDENT-FILE-CONTENT"):
                print("FAILED: resident->nonres convert"); sys.exit(1)
            if "processed successfully" not in run(["ntfsfix", "-n", cvimg]).stdout:
                print("FAILED: convert (volume not clean)"); sys.exit(1)
            print("resident->nonresident: OK (ntfs-3g read-back + clean volume)")

            # directory B-tree leaf split: create many files, forcing split(s)
            mexe = os.path.join(tmp, "ntfsmany")
            run(["cc", "-O1", "-I", SRC, os.path.join(HERE, "ntfsmany.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", mexe])
            mimg = os.path.join(tmp, "many.img"); run([fexe, mimg, "128", "MANY"])
            run([mexe, mimg, "45"])
            ls = run(["ntfsls", mimg]).stdout
            nfiles = sum(1 for i in range(45) if ("file%04d.txt" % i) in ls)
            if nfiles != 45: print("FAILED: leaf-split (%d/45 listed)" % nfiles); sys.exit(1)
            if run(["ntfscat", mimg, "file0000.txt"]).stdout.strip() != "content-of-file-0000":
                print("FAILED: leaf-split (content)"); sys.exit(1)
            if "processed successfully" not in run(["ntfsfix", "-n", mimg]).stdout:
                print("FAILED: leaf-split (volume not clean)"); sys.exit(1)
            print("leaf-split: OK (45 files across split B-tree + clean volume)")

            # small (resident) $INDEX_ROOT insert/remove in $Extend
            sexe = os.path.join(tmp, "ntfssmall")
            run(["cc", "-O1", "-I", SRC, os.path.join(HERE, "ntfssmall.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", sexe])
            simg = os.path.join(tmp, "small.img"); run([fexe, simg, "64", "SMALLIDX"])
            run([sexe, simg])
            if "processed successfully" not in run(["ntfsfix", "-n", simg]).stdout:
                print("FAILED: small-index (volume not clean)"); sys.exit(1)
            print("small-index: OK (resident $INDEX_ROOT insert+remove, clean volume)")

        # ADS: write a named stream with ntfscp -N, read it through our parser
        if shutil.which("ntfscp") and shutil.which("ntfsls"):
            aexe = os.path.join(tmp, "ntfsads")
            run(["cc", "-O1", "-I", SRC, os.path.join(HERE, "ntfsads.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", aexe])
            aimg = os.path.join(tmp, "ads.img")
            with open(aimg, "wb") as f: f.truncate(32 * 1024 * 1024)
            run(["mkntfs", "-Q", "-F", "-L", "ADS", aimg])
            with open(os.path.join(tmp, "m.txt"), "w") as f: f.write("main stream")
            with open(os.path.join(tmp, "s.txt"), "w") as f: f.write("SECRET-ADS-DATA")
            run(["ntfscp", aimg, os.path.join(tmp, "m.txt"), "f.txt"])
            run(["ntfscp", "-N", "secret", aimg, os.path.join(tmp, "s.txt"), "f.txt"])
            inode = None
            for line in run(["ntfsls", "-i", aimg]).stdout.splitlines():
                if "f.txt" in line: inode = line.split()[0]
            out = run([aexe, aimg, inode, "secret"]).stdout
            if "SECRET-ADS-DATA" not in out or ":secret" not in out:
                print("FAILED: ADS (stream not read)"); sys.exit(1)
            print("ADS: OK (named stream enumerated + read)")

        # directory B-tree: the driver's src/ntfsidx.c run in user mode on an
        # ntfsfmt volume - random inserts/deletes in the root directory (deep
        # trees with long names), all invariants checked as it goes
        if os.path.exists(os.path.join(FMT, "ntfsfmt.c")):
            fexe = os.path.join(tmp, "ntfsfmt")
            if not os.path.exists(fexe):
                run(["cc", "-O1", "-I", SRC, os.path.join(FMT, "ntfsfmt.c"), "-o", fexe])
            bexe = os.path.join(tmp, "ntfsbtree")
            run(["cc", "-O1", "-DNTFS_IDX_TEST", "-I", SRC, "-I", HERE,
                 os.path.join(HERE, "ntfsbtree.c"), os.path.join(SRC, "ntfsidx.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", bexe])
            for seed, ops, maxlen in ((1, 8000, 20), (2, 6000, 200)):
                bimg = os.path.join(tmp, "bt.img")
                if os.path.exists(bimg): os.remove(bimg)
                run([fexe, bimg, "128", "BT"])
                out = run([bexe, bimg, "5", str(seed), str(ops), str(maxlen)]).stdout
                if "PASS" not in out:
                    print("FAILED: btree"); print(out); sys.exit(1)
                if "processed successfully" not in run(["ntfsfix", "-n", bimg]).stdout:
                    print("FAILED: btree (volume not clean)"); sys.exit(1)
            print("btree: OK (multi-level index insert/split/delete/merge, invariants + ntfsfix)")

        # transparent LZNT1 compression (the COMPRESS direction): compress a
        # plain file into an image with the driver's own layout (ntfsmkcomp uses
        # src/ntfssup.c: NtfsLznt1CompressUnit + NtfsSetBaseDataRuns) and check
        # ntfs-3g reads every byte back - proves the on-disk compressed attribute
        # and runs are what a real NTFS reader expects.
        if shutil.which("ntfscp") and shutil.which("ntfsls"):
            cexe = os.path.join(tmp, "ntfsmkcomp")
            run(["cc", "-O1", "-w", "-I", SRC, os.path.join(HERE, "ntfsmkcomp.c"),
                 os.path.join(SRC, "ntfssup.c"), "-o", cexe])
            payloads = {
                "text":   b"The quick brown fox jumps over the lazy dog. " * 8000,
                "zeros":  bytes(200000),
                "random": os.urandom(100000),
                "small":  b"A" * 60000,
            }
            for name, data in payloads.items():
                cimg = os.path.join(tmp, "cmp.img")
                if os.path.exists(cimg): os.remove(cimg)
                with open(cimg, "wb") as f: f.truncate(80 * 1024 * 1024)
                run(["mkntfs", "-Q", "-F", "-c", "4096", "-L", "CMP", cimg])
                pf = os.path.join(tmp, "p.bin")
                with open(pf, "wb") as f: f.write(data)
                run(["ntfscp", cimg, pf, "c.bin"])
                inode = None
                for line in run(["ntfsls", "-i", cimg]).stdout.splitlines():
                    if "c.bin" in line: inode = line.split()[0]
                run([cexe, cimg, inode])
                mp = os.path.join(tmp, "cmnt"); os.makedirs(mp, exist_ok=True)
                try:
                    run(["ntfs-3g", cimg, mp])
                    with open(os.path.join(mp, "c.bin"), "rb") as f: back = f.read()
                finally:
                    subprocess.run(["fusermount", "-u", mp])
                if back != data:
                    print("FAILED: compression (%s: ntfs-3g read-back differs)" % name); sys.exit(1)
            print("compression: OK (LZNT1 compress -> ntfs-3g reads back identical: %s)"
                  % ", ".join(payloads))

        print("ALL PASS")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    main()
