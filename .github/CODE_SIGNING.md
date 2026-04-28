# GitHub Actions Code Signing Setup

This document explains how to set up code signing for automated macOS installer builds.

## GitHub Secrets Setup

### Repository Secrets vs Environment Secrets

For code signing certificates, use **Repository secrets** (not Environment secrets) because:

- **Repository secrets**: Available to all workflows in the repo, suitable for release builds
- **Environment secrets**: Scoped to specific environments (production/staging), require manual approval

Since your workflow triggers on release publication (a production event), **Repository secrets** are appropriate and simpler to manage.

**When to use Environment secrets**: If you have multiple deployment environments (staging/production) and want different certificates or require manual approval for production releases.

### Adding Repository Secrets

Go to: **Settings → Secrets and variables → Actions → Repository secrets**

Add each secret listed below:

### Certificate Secrets
- `CODESIGN_CERTIFICATE_P12`: Your Developer ID certificate exported as base64-encoded PKCS#12 (.p12) file
- `CODESIGN_CERTIFICATE_PASSWORD`: Password for the PKCS#12 certificate file
- `KEYCHAIN_PASSWORD`: Password for the temporary keychain (can be any secure password)

### Certificate identity secrets
- `CODESIGN_APP`: Use your **Developer ID Application** certificate name here
- `CODESIGN_INSTALLER`: Use your **Developer ID Installer** certificate name here

> Note: `CODESIGN_CERTIFICATE_P12` is the actual base64-encoded `.p12` file payload. `CODESIGN_APP` and `CODESIGN_INSTALLER` are just the certificate identity names used by the signing commands, not separate base64 secrets.

### Windows Certificate Secrets
- `WINDOWS_CODESIGN_SUBJECT`: Certificate subject name (e.g., "Your Name")
- `WINDOWS_CODESIGN_PFX`: Base64-encoded .pfx certificate file (optional, leave empty to use certificate store)
- `WINDOWS_CODESIGN_PFX_PASSWORD`: Password for the .pfx file (if using PFX)

### Notarization Secrets
- `NOTARIZE_APPLE_ID`: Your Apple ID email address
- `NOTARIZE_PASSWORD`: App-specific password for notarytool (generate at https://appleid.apple.com)
- `NOTARIZE_TEAM_ID`: Your Apple Developer Team ID

## How to Export Your Certificate

### macOS Certificate - Alternative Methods

If you can't export as .p12 from Keychain Access, try these alternatives:

#### Method 1: Command Line Export
```bash
# Find your certificate name first
security find-identity -v -p codesigning

# Export using the certificate name (replace with your actual name)
security export -k ~/Library/Keychains/login.keychain-db -t identities -f pkcs12 -o certificate.p12
```

#### Method 2: Use Keychain Access GUI
1. Open Keychain Access
2. Go to **login** keychain → **My Certificates**
3. Find your "Developer ID Application: Your Name (TEAMID)" certificate
4. **Important**: Make sure the certificate shows a triangle that expands to show the private key
5. If you see the private key, right-click the certificate → **Export**
6. Choose **Personal Information Exchange (.p12)** format
7. Set a strong password

#### Method 3: Check Certificate Validity
```bash
# Verify your certificate is properly installed
security find-identity -v -p codesigning | grep "Developer ID"

# Check if certificate has private key (should show "private key available")
security find-certificate -c "Developer ID Application" -p
```

#### Method 4: Re-download from Apple Developer
If export fails, you may need to:
1. Go to [Apple Developer Certificates page](https://developer.apple.com/account/resources/certificates/list)
2. Revoke the old certificate
3. Create a new Developer ID certificate
4. Download and install the new certificate

### Troubleshooting
- **"Export not available"**: Certificate might not have private key - re-download from Apple Developer
- **"Wrong format"**: Make sure to choose .p12 (PKCS#12) format, not .cer
- **Permission denied**: Run Keychain Access as administrator
- **No private key visible**: The certificate was installed without the private key - you need to re-download it from Apple Developer

### If You Don't Have the Private Key
Developer ID certificates must include the private key to be usable for code signing. If your certificate doesn't have a private key:

1. Go to [Apple Developer Certificates](https://developer.apple.com/account/resources/certificates/list)
2. Find your Developer ID certificates
3. Click the download button to get new .cer files
4. Double-click the downloaded .cer files to install them in Keychain Access
5. The newly installed certificates should now have private keys and be exportable

### Windows Certificate
1. Open Certificate Manager (certmgr.msc)
2. Find your code signing certificate in Personal → Certificates
3. Right-click → All Tasks → Export
4. Choose "Yes, export the private key" → PKCS #12 (.pfx)
5. Set a password and save the .pfx file
6. Convert to base64:
   ```bash
   base64 -i certificate.pfx
   ```
7. Copy the base64 output to the `WINDOWS_CODESIGN_PFX` secret

## Certificate Requirements

- You must have an active Apple Developer Program membership
- The certificates must be valid (not expired)
- The certificates must be installed on your development machine for local testing

## Workflow Behavior

- **macOS**: If certificate secrets are configured → signed + notarized installers; otherwise → unsigned installers
- **Windows**: If certificate secrets are configured → signed installers; otherwise → unsigned installers

## Security Notes

- Never commit certificate files or passwords to your repository
- Rotate app-specific passwords regularly
- Use a dedicated Apple ID for CI if possible
- The temporary keychain is deleted after each build