//! Local retained port-list preferences. Explicit scans add every present port;
//! connection state and logs are never persisted as authority.
use crate::Port;
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap, HashSet},
    path::PathBuf,
    sync::Mutex,
};

#[derive(Clone, Deserialize, Serialize)]
struct Record {
    id: String,
    name: String,
    usb_vendor_id: Option<u16>,
    usb_product_id: Option<u16>,
    usb_serial_number: Option<String>,
    removed: bool,
}
#[derive(Clone, Deserialize, Serialize)]
struct Saved {
    version: u32,
    ports: BTreeMap<String, Record>,
    #[serde(default)]
    ambiguous_serials: BTreeSet<String>,
}
impl Default for Saved {
    fn default() -> Self {
        Self {
            version: 1,
            ports: BTreeMap::new(),
            ambiguous_serials: BTreeSet::new(),
        }
    }
}
pub struct Preferences {
    path: Option<PathBuf>,
    saved: Mutex<Saved>,
}
impl Default for Preferences {
    fn default() -> Self {
        Self {
            path: None,
            saved: Mutex::new(Saved::default()),
        }
    }
}
impl Preferences {
    pub fn open(path: PathBuf) -> Result<Self, String> {
        let saved = match std::fs::read(&path) {
            Ok(bytes) => {
                if bytes.len() > 1024 * 1024 {
                    return Err("串口记录文件过大。".into());
                }
                let value: Saved =
                    serde_json::from_slice(&bytes).map_err(|e| format!("无法读取串口记录：{e}"))?;
                if value.version != 1 {
                    return Err("串口记录版本不受支持。".into());
                }
                value
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Saved::default(),
            Err(error) => return Err(error.to_string()),
        };
        Ok(Self {
            path: Some(path),
            saved: Mutex::new(saved),
        })
    }
    fn persist(&self, saved: &Saved) -> Result<(), String> {
        let Some(path) = self.path.as_ref() else {
            return Ok(());
        };
        let parent = path.parent().ok_or("串口记录路径无效。")?;
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        let mut file = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
        serde_json::to_writer_pretty(&mut file, saved).map_err(|e| e.to_string())?;
        file.as_file().sync_all().map_err(|e| e.to_string())?;
        file.persist(path).map_err(|e| e.to_string())?;
        Ok(())
    }
    fn usb_key(port: &Port) -> Option<String> {
        if let (Some(vid), Some(pid), Some(serial)) = (
            port.usb_vendor_id,
            port.usb_product_id,
            port.usb_serial_number.as_ref().filter(|s| !s.is_empty()),
        ) {
            return Some(format!("usb:{vid:04x}:{pid:04x}:{serial}"));
        }
        None
    }
    fn record(port: &Port) -> Record {
        Record {
            id: port.id.clone(),
            name: port.name.clone(),
            usb_vendor_id: port.usb_vendor_id,
            usb_product_id: port.usb_product_id,
            usb_serial_number: port.usb_serial_number.clone(),
            removed: false,
        }
    }
    fn port(record: &Record) -> Port {
        Port {
            id: record.id.clone(),
            name: record.name.clone(),
            usb_vendor_id: record.usb_vendor_id,
            usb_product_id: record.usb_product_id,
            usb_serial_number: record.usb_serial_number.clone(),
            selected_by_default: true,
            initial_status: None,
            initial_error: None,
            present: false,
            remembered: true,
            connected: false,
            active: false,
        }
    }
    pub fn scan(&self, discovered: Vec<Port>, add_new: bool) -> Result<Vec<Port>, String> {
        let mut saved = self.saved.lock().unwrap();
        let mut next = saved.clone();
        let mut counts = HashMap::new();
        for port in &discovered {
            if let Some(key) = Self::usb_key(port) {
                *counts.entry(key).or_insert(0) += 1;
            }
        }
        next.ambiguous_serials.extend(
            counts
                .into_iter()
                .filter_map(|(key, count)| (count > 1).then_some(key)),
        );
        // Once a serial is known to be duplicated, retain endpoint identity even
        // after one device is unplugged. Otherwise an exclusion could migrate
        // to a different device as the inventory changes.
        let ambiguous: Vec<_> = next
            .ports
            .keys()
            .filter(|key| next.ambiguous_serials.contains(*key))
            .cloned()
            .collect();
        for key in ambiguous {
            let record = next.ports.remove(&key).unwrap();
            next.ports
                .entry(format!("path:{}", record.id))
                .and_modify(|existing| existing.removed |= record.removed)
                .or_insert(record);
        }
        let mut visible = Vec::new();
        let mut seen = HashSet::new();
        for mut port in discovered {
            if seen.contains(&port.id) {
                continue;
            }
            let usb_key = Self::usb_key(&port).filter(|key| !next.ambiguous_serials.contains(key));
            let mut key = usb_key
                .clone()
                .unwrap_or_else(|| format!("path:{}", port.id));
            let previous = next
                .ports
                .get_key_value(&key)
                .or_else(|| {
                    next.ports.iter().find(|(_, record)| {
                        record.id == port.id
                            && (usb_key.is_none()
                                || record
                                    .usb_serial_number
                                    .as_deref()
                                    .is_none_or(str::is_empty))
                    })
                })
                .map(|(key, record)| (key.clone(), record.clone()));
            if !add_new && previous.as_ref().is_none_or(|(_, r)| r.removed) {
                continue;
            }
            port.present = true;
            port.remembered = previous.as_ref().is_some_and(|(_, r)| !r.removed);
            let mut record = Self::record(&port);
            if let Some((previous_key, previous)) = previous {
                // Missing descriptor fields must not erase a known unique USB
                // identity or create a second offline copy of the same endpoint.
                if usb_key.is_none() && previous_key.starts_with("usb:") {
                    key = previous_key.clone();
                    record.usb_vendor_id = record.usb_vendor_id.or(previous.usb_vendor_id);
                    record.usb_product_id = record.usb_product_id.or(previous.usb_product_id);
                    record.usb_serial_number =
                        record.usb_serial_number.or(previous.usb_serial_number);
                }
                next.ports.remove(&previous_key);
            }
            next.ports.insert(key, record);
            seen.insert(port.id.clone());
            visible.push(port);
        }
        for record in next.ports.values().filter(|r| !r.removed) {
            if seen.insert(record.id.clone()) {
                visible.push(Self::port(record));
            }
        }
        visible.sort_by(|a, b| a.id.cmp(&b.id));
        self.persist(&next)?;
        *saved = next;
        Ok(visible)
    }
    pub fn remove(&self, id: &str) -> Result<(), String> {
        let mut saved = self.saved.lock().unwrap();
        let mut next = saved.clone();
        let mut found = false;
        for record in next.ports.values_mut().filter(|r| r.id == id) {
            record.removed = true;
            found = true;
        }
        if !found {
            return Err("串口不在列表中，请重新扫描。".into());
        }
        self.persist(&next)?;
        *saved = next;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn port(id: &str, serial: Option<&str>) -> Port {
        Port {
            id: id.into(),
            name: "USB Serial".into(),
            usb_vendor_id: Some(1),
            usb_product_id: Some(2),
            usb_serial_number: serial.map(String::from),
            selected_by_default: true,
            initial_status: None,
            initial_error: None,
            present: true,
            remembered: false,
            connected: false,
            active: false,
        }
    }
    #[test]
    fn startup_retains_removal_and_explicit_scan_adds_all_present_ports() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("ports.json");
        let prefs = Preferences::open(path.clone()).unwrap();
        let first = prefs
            .scan(vec![port("COM1", Some("a")), port("COM2", None)], true)
            .unwrap();
        assert!(first.iter().all(|p| !p.remembered));
        prefs.remove("COM1").unwrap();
        let prefs = Preferences::open(path).unwrap();
        let restored = prefs
            .scan(
                vec![
                    port("COM9", Some("a")),
                    port("COM2", None),
                    port("COM3", Some("new")),
                ],
                false,
            )
            .unwrap();
        assert_eq!(restored.len(), 1);
        assert_eq!(restored[0].id, "COM2");
        assert!(restored[0].remembered);
        let scanned = prefs
            .scan(
                vec![
                    port("COM9", Some("a")),
                    port("COM2", None),
                    port("COM3", Some("new")),
                ],
                true,
            )
            .unwrap();
        assert_eq!(scanned.len(), 3);
        assert!(scanned
            .iter()
            .any(|p| p.id == "COM9" && p.present && !p.remembered));
        assert!(prefs
            .scan(vec![port("COM9", Some("a"))], true)
            .unwrap()
            .iter()
            .any(|p| p.id == "COM9" && p.present));
    }
    #[test]
    fn duplicate_usb_serials_keep_independent_retained_rows() {
        let prefs = Preferences::default();
        prefs
            .scan(
                vec![port("COM1", Some("same")), port("COM2", Some("same"))],
                true,
            )
            .unwrap();
        prefs.remove("COM1").unwrap();
        let startup = prefs
            .scan(
                vec![port("COM1", Some("same")), port("COM2", Some("same"))],
                false,
            )
            .unwrap();
        assert_eq!(startup.len(), 1);
        assert_eq!(startup[0].id, "COM2");
        let single = prefs.scan(vec![port("COM2", Some("same"))], false).unwrap();
        assert_eq!(single.len(), 1);
        assert!(single[0].present);
        let offline = prefs.scan(vec![port("COM1", Some("same"))], false).unwrap();
        assert_eq!(offline.len(), 1);
        assert_eq!(offline[0].id, "COM2");
        assert!(!offline[0].present);
        let scanned = prefs
            .scan(
                vec![port("COM1", Some("same")), port("COM2", Some("same"))],
                true,
            )
            .unwrap();
        assert_eq!(scanned.len(), 2);
        assert!(scanned.iter().all(|p| p.present));
    }
    #[test]
    fn descriptor_changes_preserve_startup_removal_but_not_explicit_scan_exclusions() {
        let prefs = Preferences::default();
        prefs.scan(vec![port("COM1", Some("a"))], true).unwrap();
        let incomplete = prefs.scan(vec![port("COM1", None)], false).unwrap();
        assert_eq!(incomplete.len(), 1);
        prefs.remove("COM1").unwrap();
        assert!(prefs
            .scan(vec![port("COM1", None)], false)
            .unwrap()
            .is_empty());
        let duplicate = prefs
            .scan(
                vec![port("COM1", Some("a")), port("COM2", Some("a"))],
                false,
            )
            .unwrap();
        assert!(duplicate.is_empty());
        let scanned = prefs
            .scan(vec![port("COM1", Some("a")), port("COM2", Some("a"))], true)
            .unwrap();
        assert_eq!(scanned.len(), 2);
        assert!(scanned.iter().all(|p| !p.remembered));
    }
    #[test]
    fn failed_save_keeps_the_previous_in_memory_list() {
        let dir = tempfile::tempdir().unwrap();
        let blocker = dir.path().join("not-a-directory");
        let prefs = Preferences {
            path: Some(blocker.join("ports.json")),
            saved: Mutex::new(Saved::default()),
        };
        std::fs::write(&blocker, "block").unwrap();
        assert!(prefs.scan(vec![port("COM1", None)], true).is_err());
        assert!(prefs.saved.lock().unwrap().ports.is_empty());
    }
}
