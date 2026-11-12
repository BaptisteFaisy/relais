//! A minimal JSON reader, only what the differential-test driver emits. The C
//! reference (`host/difftest/bridge.c`) carries its own equivalent DOM for the
//! same reason: the port pulls no crate, so the harness parses by hand. This
//! module is `std`-only and is NOT part of the protocol — the library above it
//! stays `no_std` and allocation free.

use std::string::String;
use std::vec::Vec;

/// One JSON value.
#[derive(Clone, Debug, PartialEq)]
pub enum JVal {
    /// `null`.
    Null,
    /// `true` / `false`.
    Bool(bool),
    /// Every number, as f64 like the C bridge does.
    Num(f64),
    /// A string.
    Str(String),
    /// An array.
    Arr(Vec<JVal>),
    /// An object, kept in source order.
    Obj(Vec<(String, JVal)>),
}

impl JVal {
    /// Field of an object, or `None`.
    #[must_use]
    pub fn get(&self, key: &str) -> Option<&JVal> {
        match self {
            JVal::Obj(fields) => fields.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// Numeric field with a default, like the C `jnum`.
    #[must_use]
    pub fn num_or(&self, key: &str, default: f64) -> f64 {
        match self.get(key) {
            Some(JVal::Num(n)) => *n,
            _ => default,
        }
    }

    /// Boolean field with a default, like the C `jbool`.
    #[must_use]
    pub fn bool_or(&self, key: &str, default: bool) -> bool {
        match self.get(key) {
            Some(JVal::Bool(b)) => *b,
            _ => default,
        }
    }

    /// String command name, if this value is an object with that field.
    #[must_use]
    pub fn cmd(&self) -> Option<&str> {
        match self.get("cmd") {
            Some(JVal::Str(s)) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Array field, or an empty slice.
    #[must_use]
    pub fn arr_or(&self, key: &str) -> &[JVal] {
        match self.get(key) {
            Some(JVal::Arr(a)) => a.as_slice(),
            _ => &[],
        }
    }
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

/// Parse one JSON value; `None` when the input is malformed. The driver sends
/// one object per line, so a strict parser is enough (ROB-09: a malformed line
/// is refused, never guessed).
#[must_use]
pub fn parse(text: &str) -> Option<JVal> {
    let mut p = Parser { s: text.as_bytes(), i: 0 };
    p.skip_ws();
    let v = p.value()?;
    p.skip_ws();
    Some(v)
}

impl<'a> Parser<'a> {
    fn peek(&self) -> u8 {
        *self.s.get(self.i).unwrap_or(&0)
    }

    fn skip_ws(&mut self) {
        while matches!(self.peek(), b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn eat(&mut self, c: u8) -> bool {
        if self.peek() == c {
            self.i += 1;
            return true;
        }
        false
    }

    fn value(&mut self) -> Option<JVal> {
        self.skip_ws();
        match self.peek() {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => self.string().map(JVal::Str),
            b't' => self.literal("true").then_some(JVal::Bool(true)),
            b'f' => self.literal("false").then_some(JVal::Bool(false)),
            b'n' => self.literal("null").then_some(JVal::Null),
            _ => self.number(),
        }
    }

    fn literal(&mut self, word: &str) -> bool {
        let end = self.i + word.len();
        if end <= self.s.len() && &self.s[self.i..end] == word.as_bytes() {
            self.i = end;
            return true;
        }
        false
    }

    fn number(&mut self) -> Option<JVal> {
        let start = self.i;
        while matches!(self.peek(), b'0'..=b'9' | b'-' | b'+' | b'.' | b'e' | b'E') {
            self.i += 1;
        }
        if self.i == start {
            return None;
        }
        std::str::from_utf8(&self.s[start..self.i]).ok()?.parse::<f64>().ok().map(JVal::Num)
    }

    fn string(&mut self) -> Option<String> {
        if !self.eat(b'"') {
            return None;
        }
        let mut out = String::new();
        loop {
            match self.peek() {
                0 => return None,
                b'"' => {
                    self.i += 1;
                    return Some(out);
                }
                b'\\' => {
                    self.i += 1;
                    let c = self.peek();
                    self.i += 1;
                    out.push(if c == b'n' { '\n' } else { c as char });
                }
                _ => {
                    out.push(self.peek() as char);
                    self.i += 1;
                }
            }
        }
    }

    fn array(&mut self) -> Option<JVal> {
        self.i += 1; /* '[' */
        let mut items = Vec::new();
        self.skip_ws();
        if self.eat(b']') {
            return Some(JVal::Arr(items));
        }
        loop {
            items.push(self.value()?);
            self.skip_ws();
            if self.eat(b',') {
                continue;
            }
            return self.eat(b']').then_some(JVal::Arr(items));
        }
    }

    fn object(&mut self) -> Option<JVal> {
        self.i += 1; /* '{' */
        let mut fields = Vec::new();
        self.skip_ws();
        if self.eat(b'}') {
            return Some(JVal::Obj(fields));
        }
        loop {
            self.skip_ws();
            let key = self.string()?;
            self.skip_ws();
            if !self.eat(b':') {
                return None;
            }
            fields.push((key, self.value()?));
            self.skip_ws();
            if self.eat(b',') {
                continue;
            }
            return self.eat(b'}').then_some(JVal::Obj(fields));
        }
    }
}
