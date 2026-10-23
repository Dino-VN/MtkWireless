//! Little/big-endian cursor over byte slices.

use crate::{Error, Result};

/// Read cursor. All reads advance the position only on success.
#[derive(Debug, Clone)]
pub struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    pub const fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    pub const fn position(&self) -> usize {
        self.pos
    }

    pub const fn remaining(&self) -> usize {
        self.data.len() - self.pos
    }

    pub const fn is_empty(&self) -> bool {
        self.remaining() == 0
    }

    pub fn seek(&mut self, pos: usize) -> Result<()> {
        if pos > self.data.len() {
            return Err(Error::Truncated {
                offset: pos,
                need: 0,
            });
        }
        self.pos = pos;
        Ok(())
    }

    pub fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        let bytes = slice_at(self.data, self.pos, n)?;
        self.pos += n;
        Ok(bytes)
    }

    pub fn skip(&mut self, n: usize) -> Result<()> {
        self.take(n).map(|_| ())
    }

    pub fn array<const N: usize>(&mut self) -> Result<[u8; N]> {
        let mut out = [0; N];
        out.copy_from_slice(self.take(N)?);
        Ok(out)
    }

    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.array::<1>()?[0])
    }

    pub fn le16(&mut self) -> Result<u16> {
        self.array().map(u16::from_le_bytes)
    }

    pub fn le32(&mut self) -> Result<u32> {
        self.array().map(u32::from_le_bytes)
    }

    pub fn be16(&mut self) -> Result<u16> {
        self.array().map(u16::from_be_bytes)
    }

    pub fn be32(&mut self) -> Result<u32> {
        self.array().map(u32::from_be_bytes)
    }
}

/// `data[offset..offset + len]`, with overflow-safe bounds checking.
pub fn slice_at(data: &[u8], offset: usize, len: usize) -> Result<&[u8]> {
    offset
        .checked_add(len)
        .and_then(|end| data.get(offset..end))
        .ok_or(Error::Truncated { offset, need: len })
}

pub fn le16_at(data: &[u8], offset: usize) -> Result<u16> {
    let mut r = Reader::new(data);
    r.seek(offset)?;
    r.le16()
}

pub fn le32_at(data: &[u8], offset: usize) -> Result<u32> {
    let mut r = Reader::new(data);
    r.seek(offset)?;
    r.le32()
}

/// Write cursor over a caller-provided buffer.
#[derive(Debug)]
pub struct Writer<'a> {
    buf: &'a mut [u8],
    len: usize,
}

impl<'a> Writer<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Self { buf, len: 0 }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn written(&self) -> &[u8] {
        &self.buf[..self.len]
    }

    pub fn put(&mut self, bytes: &[u8]) -> Result<()> {
        let room = self.buf.len() - self.len;
        if bytes.len() > room {
            return Err(Error::Overflow {
                need: bytes.len(),
                room,
            });
        }
        self.buf[self.len..self.len + bytes.len()].copy_from_slice(bytes);
        self.len += bytes.len();
        Ok(())
    }

    pub fn u8(&mut self, v: u8) -> Result<()> {
        self.put(&[v])
    }

    pub fn le16(&mut self, v: u16) -> Result<()> {
        self.put(&v.to_le_bytes())
    }

    pub fn le32(&mut self, v: u32) -> Result<()> {
        self.put(&v.to_le_bytes())
    }

    pub fn be16(&mut self, v: u16) -> Result<()> {
        self.put(&v.to_be_bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_in_order() {
        let mut r = Reader::new(&[0x01, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xab, 0xcd]);
        assert_eq!(r.u8(), Ok(1));
        assert_eq!(r.le16(), Ok(0x1234));
        assert_eq!(r.le32(), Ok(0x1234_5678));
        assert_eq!(r.be16(), Ok(0xabcd));
        assert!(r.is_empty());
    }

    #[test]
    fn short_read_fails_without_advancing() {
        let mut r = Reader::new(&[1, 2, 3]);
        r.skip(2).unwrap();
        assert_eq!(r.le16(), Err(Error::Truncated { offset: 2, need: 2 }));
        assert_eq!(r.position(), 2);
        assert_eq!(r.u8(), Ok(3));
    }

    #[test]
    fn offsets_never_overflow() {
        assert!(slice_at(&[0; 4], usize::MAX, 2).is_err());
        assert!(le32_at(&[0; 4], 1).is_err());
        assert_eq!(le32_at(&[1, 0, 0, 0], 0), Ok(1));
    }

    #[test]
    fn writer_stops_at_capacity() {
        let mut buf = [0; 3];
        let mut w = Writer::new(&mut buf);
        w.le16(0xbeef).unwrap();
        assert_eq!(w.le16(1), Err(Error::Overflow { need: 2, room: 1 }));
        w.u8(7).unwrap();
        assert_eq!(w.written(), &[0xef, 0xbe, 7]);
    }
}
