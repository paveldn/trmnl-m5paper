const modelConfirmation = document.querySelector("#model-confirmation");
const installButton = document.querySelector("#install-button");

function updateInstallButton() {
  installButton.disabled = !modelConfirmation.checked;
}

modelConfirmation.addEventListener("change", updateInstallButton);
updateInstallButton();
