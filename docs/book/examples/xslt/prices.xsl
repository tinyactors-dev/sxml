<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:money="https://example.org/money">
  <xsl:use-package name="money.xsl" package-version="1.0"/>
  <xsl:output method="text"/>

  <xsl:template match="/">
    <xsl:for-each select="//book">
      <xsl:text>{title}: {money:format(money:in-euros(price))}&#10;</xsl:text>
    </xsl:for-each>
    <xsl:text>Total: {money:format(sum(//price ! money:in-euros(.)))}&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
