<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>
  <xsl:template match="receipt">
    <table>
      <xsl:apply-templates select="item"/>
      <tr>
        <th colspan="2">Total</th>
        <td>{format-number(sum(item/@amount), '0.00')}</td>
      </tr>
    </table>
  </xsl:template>
  <xsl:template match="item">
    <tr>
      <td>{.}</td>
      <td>{@quantity}</td>
      <td>{format-number(@amount, '0.00')}</td>
    </tr>
  </xsl:template>
</xsl:stylesheet>
