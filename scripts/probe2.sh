#!/usr/bin/env bash
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36"

echo "== apt.devkitpro.org over IPv4 =="
curl -4 -s -o /dev/null -w "code=%{http_code}\n" -A "$UA" \
  -H "Accept: text/html,application/xhtml+xml" --max-time 25 https://apt.devkitpro.org/ || echo "curl-failed"

echo "== apt install script IPv4 with full headers =="
curl -4 -s -o /dev/null -w "code=%{http_code}\n" -A "$UA" \
  -H "Accept: */*" --max-time 25 https://apt.devkitpro.org/install-devkitpro-pacman || echo "curl-failed"

echo "== downloads.devkitpro.org redirect target =="
curl -sIL -A "$UA" --max-time 25 https://downloads.devkitpro.org/ | grep -iE "^HTTP|^location" || echo "none"

echo "== docker hub reachability =="
curl -s -o /dev/null -w "code=%{http_code}\n" --max-time 25 https://registry-1.docker.io/v2/ || echo "curl-failed"
curl -s -o /dev/null -w "code=%{http_code}\n" --max-time 25 https://auth.docker.io/token || echo "curl-failed"
