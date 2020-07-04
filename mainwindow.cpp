#include <RPN_parser.h>
//
#include "mainwindow.h"
#include "ui_mainwindow.h"

int test02(void);

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
  ui->setupUi(this);
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::on_expression_returnPressed() {
  RPN_parser rpn(ui->expression->text().toStdString());

  ui->statusbar->showMessage(rpn.ok() ? "ok" : "syntax error");

  if (rpn.ok()) {
    ui->code->setText(QString::fromStdString(rpn.code));

    ui->disp->clear();
    for (float x = ui->sb_from->value(); x < ui->sb_to->value();
         x += ui->sb_inc->value())
      ui->disp->append(QString("%1 : %2").arg(x).arg(rpn.evaluate(x)));
  }
}
