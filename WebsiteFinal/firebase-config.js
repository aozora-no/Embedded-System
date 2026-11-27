firebase.initializeApp({
  apiKey: "AIzaSyDXMgYgjPJ7BDXhO2Tb-9H_StmtwVFW7k4",
  authDomain: "e3embeddedsystem-4e47e.firebaseapp.com",
  databaseURL: "https://e3embeddedsystem-4e47e-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "e3embeddedsystem-4e47e"
});

const auth = firebase.auth();
const db = firebase.database();

const device = db.ref("devices/HM");

const serverTimestamp =
  firebase.database.ServerValue.TIMESTAMP;

async function approved(user) {

  return user &&
    (await db.ref("approvedUsers/" + user.uid)
    .once("value")).val() === true;

}
